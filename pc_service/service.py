"""Stage 2: poll Anthropic on a timer, serve a trimmed summary to the LAN.

Two halves that never touch each other's internals:

  the poller   -- reads the token, calls Anthropic, keeps the last good
                  result in memory. The only half that knows a credential
                  exists. (This is the half a future cloud relay replaces.)
  the server   -- answers GET /usage on the LAN with derived numbers only.
                  Never sees the token.

The token is read at runtime from Claude Code's own credential file, sent
only to api.anthropic.com, and never logged or served. See ../SECRETS.md.

THREADS -- there are three kinds, and knowing which is which explains most
of the design decisions below:

  main thread    -- parses arguments, binds the socket, then hands itself to
                    serve_forever() and does nothing else.
  poller thread  -- one, started in main(). Runs poll_forever() in a loop for
                    the life of the process.
  handler threads-- one per incoming HTTP connection, created by
                    ThreadingHTTPServer as requests arrive.

The poller writes UsageCache; the handler threads read it. That is the only
shared state, which is why UsageCache is the only thing here holding a lock.

READ IT IN THIS ORDER: UsageCache (the shared state), then _format_reset and
build_payload (turning Anthropic's response into our contract), then
poll_forever (the writer), then UsageHandler (the readers), then main().

Run:  python service.py
Then: curl http://<this-pc-lan-ip>:8734/usage
"""

import argparse
import datetime
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from fetch_usage import RateLimited, fetch_usage, read_access_token

DEFAULT_PORT = 8734
# The numbers move slowly, and the endpoint is undocumented -- so the interval
# is set by what upstream tolerates, not by what the display could use.
#
# This was 60s, on the evidence that two instances polling earned a 429 and one
# did not. That stopped being true: at 60s with a single instance, 13 of 45
# polls over an hour came back 429, in a metronomic two-ok-then-one-refused
# cycle. (A plausible reason it changed: Claude Code itself calls this endpoint
# for its own /status, so during an active session this service is not the only
# caller spending the budget.)
#
# 120s costs almost nothing in freshness, because the 120s backoff after each
# 429 meant the real cadence was already one *successful* poll every ~111s. The
# reason to care is not staleness but honesty: a 429 makes the service serve
# its last good payload with "stale": true, so at 60s the display would have
# flagged perfectly good two-minute-old data as stale about a third of the
# time -- and an indicator that cries wolf that often is one you learn to
# ignore, which defeats the point of having it.
#
# If 429s reappear at 120s, 180s is the next step. The real limit is unknown.
POLL_INTERVAL_SECONDS = 120
RETRY_INTERVAL_SECONDS = 20     # after a failure, recover faster than a full cycle

# A 429 is the server explicitly asking us to stop. Retrying it on the normal
# schedule makes things worse, so back off exponentially from here and reset
# only once a poll actually succeeds.
RATE_LIMIT_BACKOFF_START = 120
RATE_LIMIT_BACKOFF_MAX = 900


def log(message):
    print(f"{datetime.datetime.now():%H:%M:%S}  {message}", flush=True)


# --- shared state ------------------------------------------------------

class UsageCache:
    """Last good upstream result + whether the most recent poll succeeded.

    The one piece of state shared across threads: the poller writes it, and
    every HTTP handler thread reads it. Hence the lock -- without it a handler
    could observe a half-updated cache, e.g. new percentages paired with the
    previous timestamp, and serve a payload that never actually existed.

    The lock is held only for the few statements that touch the fields, never
    across a network call, so handlers never wait on Anthropic.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._payload = None        # the trimmed dict, minus stale/now_epoch
        self._fetched_at = None     # datetime of the last SUCCESSFUL poll
        self._stale = True          # no data yet counts as stale
        self._public_error = "no successful poll yet"

    def store_success(self, payload):
        with self._lock:
            self._payload = payload
            self._fetched_at = datetime.datetime.now()
            self._stale = False
            self._public_error = None

    def store_failure(self, public_reason):
        """Record a failure using text that is safe to serve to the LAN.

        Deliberately NOT the exception's own message: upstream error bodies
        get up to 500 characters embedded into RuntimeError by fetch_usage,
        and a 401/403 body can carry request IDs and account detail. That
        belongs in this machine's log, not in a response to any unauthenticated
        device on the network. Callers log the detailed version themselves.
        """
        with self._lock:
            self._stale = True
            self._public_error = public_reason

    def snapshot(self):
        with self._lock:
            return self._payload, self._fetched_at, self._stale, self._public_error


# --- turning the upstream response into our contract -------------------

def _round_to_minute(dt):
    """Round to the nearest minute so 15:39:59.6 displays as 15:40, not 15:39."""
    return (dt + datetime.timedelta(seconds=30)).replace(second=0, microsecond=0)


def _parse_iso(iso_string):
    """Parse an upstream ISO timestamp, or None.

    Upstream sends UTC with a trailing 'Z' ("2026-09-01T17:00:00.123456Z").
    datetime.fromisoformat only learned to accept 'Z' in Python 3.11, and this
    service is documented as running on Linux too, where 3.9/3.10 are still
    common. Normalizing it here keeps one code path instead of a version
    check -- and matters because the failure is silent: the caller would
    return (None, None), build_payload would still succeed on percentages
    alone, and the display would quietly lose both reset times forever.
    """
    if not isinstance(iso_string, str):
        return None

    text = iso_string.strip()
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"

    try:
        parsed = datetime.datetime.fromisoformat(text)
    except ValueError:
        return None

    if parsed.tzinfo is None:                     # assume UTC if unmarked
        parsed = parsed.replace(tzinfo=datetime.timezone.utc)
    return parsed


def _format_reset(iso_string):
    """('17:39', 1788291599) from an upstream ISO timestamp, in PC-local time.

    Returns (None, None) if the timestamp is missing or unparseable -- the
    endpoint is undocumented, so a shape we don't recognize must not raise.
    """
    parsed = _parse_iso(iso_string)
    if parsed is None:
        return None, None

    epoch = int(parsed.timestamp())

    # Two subtleties, both about the weekday prefix rather than the time:
    #
    # 1. Decide today/not-today from the UNROUNDED local time. Rounding is a
    #    display concern, and it can cross midnight: a reset at 23:59:45
    #    rounds to 00:00, and comparing that would label a reset 15 seconds
    #    away as tomorrow ("Thu 00:00") while resets_epoch still says today.
    # 2. Take "now" in the machine's current local zone, not in the reset
    #    instant's zone. astimezone() pins the offset that applied at the
    #    reset, which for a 7-day window can sit the far side of a DST change
    #    -- enough to flip the comparison near midnight.
    local_exact = parsed.astimezone()
    now_local = datetime.datetime.now().astimezone()
    is_today = local_exact.date() == now_local.date()

    local = _round_to_minute(local_exact)
    if is_today:
        return local.strftime("%H:%M"), epoch
    return local.strftime("%a %H:%M"), epoch


def _percent(window):
    """Round 'utilization' to an int 0-100, or None if it isn't a number.

    Upstream sends this as a float (24.0), not the int you might expect, so
    rounding is required rather than cosmetic.

    The isinstance(value, bool) exclusion is not paranoia: in Python `bool` is
    a subclass of `int`, so a stray `true` in the JSON would pass the numeric
    check and quietly render as 1%. The max/min clamp guards the same class of
    surprise from the other direction -- an out-of-range number becomes a
    valid percentage rather than something the display cannot lay out.
    """
    if not isinstance(window, dict):
        return None
    value = window.get("utilization")
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    return max(0, min(100, round(value)))


def build_payload(raw):
    """Trim the upstream response down to the firmware contract.

    Raises RuntimeError if the response doesn't contain the two windows we
    need -- better to keep serving the last good data and flag it stale than
    to serve confident-looking nulls.
    """
    if not isinstance(raw, dict):
        raise RuntimeError("upstream response was not a JSON object")

    five_hour = raw.get("five_hour")
    seven_day = raw.get("seven_day")

    five_pct = _percent(five_hour)
    seven_pct = _percent(seven_day)
    if five_pct is None or seven_pct is None:
        raise RuntimeError("upstream response is missing five_hour/seven_day utilization")

    five_at, five_epoch = _format_reset((five_hour or {}).get("resets_at"))
    seven_at, seven_epoch = _format_reset((seven_day or {}).get("resets_at"))

    return {
        "five_hour_pct": five_pct,
        "five_hour_resets_at": five_at,
        "five_hour_resets_epoch": five_epoch,
        "seven_day_pct": seven_pct,
        "seven_day_resets_at": seven_at,
        "seven_day_resets_epoch": seven_epoch,
    }


# --- the poller --------------------------------------------------------

def poll_forever(cache):
    """Fetch, cache, sleep, repeat -- forever, on its own thread.

    This function must never return and never raise: it IS the data source,
    and a dead poller means a display frozen on old numbers with nothing in
    the log to say why. Every failure path therefore records the failure,
    sleeps, and continues.

    The `except` clauses are ordered deliberately. RateLimited subclasses
    RuntimeError, and Python matches except clauses top to bottom, so putting
    RuntimeError first would swallow every 429 and apply the wrong (much too
    short) retry interval. The bare `except Exception` last is the backstop
    for anything unforeseen -- an undocumented endpoint can return shapes we
    have not imagined, and none of them should kill this thread.
    """
    rate_limit_backoff = RATE_LIMIT_BACKOFF_START

    while True:
        try:
            # Re-read the token every cycle rather than caching it: Claude
            # Code refreshes it periodically, and a held copy starts 401ing
            # after a few hours.
            token = read_access_token()
            payload = build_payload(fetch_usage(token))
        except RateLimited as e:
            # Retry-After is a hint we may only extend, never shorten: this
            # endpoint has been observed sending "Retry-After: 0" alongside a
            # 429, and obeying that literally turns the backoff into a hot
            # loop hammering the server several times a second. Our own
            # exponential backoff is always the floor.
            wait = max(e.retry_after or 0, rate_limit_backoff)
            rate_limit_backoff = min(rate_limit_backoff * 2, RATE_LIMIT_BACKOFF_MAX)
            cache.store_failure("rate limited upstream")
            log(f"poll rate-limited -- backing off {wait}s")
            time.sleep(wait)
            continue
        except RuntimeError as e:
            cache.store_failure("upstream request failed")
            log(f"poll FAILED ({e}) -- retrying in {RETRY_INTERVAL_SECONDS}s")
            time.sleep(RETRY_INTERVAL_SECONDS)
            continue
        except Exception as e:                    # never let the thread die
            cache.store_failure("internal error")
            log(f"poll FAILED unexpectedly ({type(e).__name__}: {e}) -- "
                f"retrying in {RETRY_INTERVAL_SECONDS}s")
            time.sleep(RETRY_INTERVAL_SECONDS)
            continue

        cache.store_success(payload)
        rate_limit_backoff = RATE_LIMIT_BACKOFF_START      # recovered; reset
        log(f"poll ok: 5h {payload['five_hour_pct']}% (reset {payload['five_hour_resets_at']}), "
            f"7d {payload['seven_day_pct']}% (reset {payload['seven_day_resets_at']})")
        time.sleep(POLL_INTERVAL_SECONDS)


# --- the LAN server ----------------------------------------------------

class UsageServer(ThreadingHTTPServer):
    """ThreadingHTTPServer that refuses to start beside an existing instance.

    Python sets SO_REUSEADDR by default, which means different things per OS:
    on Linux it only allows rebinding a port stuck in TIME_WAIT (useful), but
    on Windows it lets a second process bind a port that is already actively
    listening. That silently produces two services polling Anthropic at twice
    the intended rate -- which is exactly how this project earned an HTTP 429.
    So: keep the flag on Linux, drop it on Windows, where a duplicate launch
    should fail loudly instead.
    """

    allow_reuse_address = sys.platform != "win32"


class UsageHandler(BaseHTTPRequestHandler):
    cache = None                                  # injected in main()
    protocol_version = "HTTP/1.1"                 # so Content-Length is honoured

    # HTTP/1.1 defaults to keep-alive, and each kept-alive connection holds a
    # thread parked in readline() waiting for the next request. Without a
    # timeout that wait is unbounded: a client that reboots or drops off WiFi
    # mid-connection never sends FIN, so the thread blocks forever (Windows
    # leaves TCP keepalive off, so nothing reaps it either). On an always-on
    # service that is one leaked thread per reflash, accumulating for the
    # life of the process.
    #
    # The firmware now sends "Connection: close", so it no longer parks a
    # thread at all -- but this timeout stays for every other client (a
    # browser tab left open on /usage does exactly what the ESP32 used to).
    timeout = 30

    def handle_one_request(self):
        """Serve one request, treating a vanished client as routine.

        A client that closes mid-connection is normal here, not exceptional:
        the ESP32 sends one request, closes, and reboots or sleeps until the
        next poll. Because HTTP/1.1 keeps the connection open, that close
        lands on a socket this thread is already reading from, and on Windows
        it surfaces as a reset (WinError 10054) rather than a clean EOF --
        which BaseHTTPRequestHandler does not catch, so it escapes as an
        unhandled exception and prints a full traceback per disconnect.

        Nothing breaks: the connection's own thread dies and the service
        carries on. But at one poll a minute this would print a traceback a
        minute, forever, and the cost of that is real -- it buries the
        failures that do matter in noise that means nothing. So catch it, note
        it at most in passing, and let the thread end quietly.

        Deliberately narrow: only the two connection-loss errors are caught.
        Anything else still escapes with its traceback intact, because a bug
        in do_GET must stay loud.
        """
        try:
            super().handle_one_request()
        except (ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True

    def _respond(self, status, body):
        encoded = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        if self.path.split("?")[0] != "/usage":
            self._respond(404, {"error": "not found", "try": "/usage"})
            return

        payload, fetched_at, stale, last_error = self.cache.snapshot()

        if payload is None:
            # Never polled successfully -- say so instead of inventing zeroes.
            self._respond(503, {"stale": True, "error": last_error or "no data yet"})
            return

        body = dict(payload)
        # updated_at is for showing; updated_epoch is for deciding. With only
        # the "HH:MM" string, a chip cannot tell five-minutes-stale from
        # five-days-stale -- three-day-old data reads as "15:04", i.e. this
        # afternoon. Paired with now_epoch, the firmware can compute the real
        # age and choose when data is too old to display at all.
        body["updated_at"] = f"{fetched_at:%H:%M}" if fetched_at else None
        body["updated_epoch"] = int(fetched_at.timestamp()) if fetched_at else None
        body["now_epoch"] = int(time.time())
        body["stale"] = stale
        self._respond(200, body)

    def log_message(self, fmt, *args):
        """Route the base class's request logging through our own log().

        Without this override BaseHTTPRequestHandler writes straight to
        stderr in its own format, so request lines and poll lines would
        interleave with different timestamps and different streams.
        """
        log(f"{self.client_address[0]} {fmt % args}")


def lan_address():
    """The address other machines on the network should use to reach us.

    Worth having because "0.0.0.0" -- what this service binds to and used to
    announce -- says "every interface" and answers nobody's actual question,
    which is what to type into the gadget. And the answer moves: a router that
    hands out addresses by DHCP can pick a different one after a reboot or a
    lease expiry, and this machine has been both .87 and .88 within two days.

    The UDP socket is the standard trick for asking the OS which interface it
    would use to reach the outside world, and reading the answer off the
    socket. Nothing is sent -- connect() on a UDP socket only fixes the route,
    it does not put a packet on the wire, so this works with the network
    unplugged from the internet and costs nothing. 8.8.8.8 is a destination,
    not a service being contacted.

    The alternative, socket.gethostbyname(socket.gethostname()), is the
    obvious-looking one and is wrong on exactly the machines where this matters:
    with several adapters -- a VM host network, a VPN, WSL -- it commonly
    returns whichever one the hostname happens to resolve to, which need not be
    the one carrying LAN traffic.

    Returns None rather than guessing if there is no route at all.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def main():
    parser = argparse.ArgumentParser(description="Serve Claude usage to the LAN.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--host", default="0.0.0.0",
                        help="0.0.0.0 exposes it to the LAN; 127.0.0.1 keeps it local")
    args = parser.parse_args()

    cache = UsageCache()
    UsageHandler.cache = cache

    # Bind before starting the poller: if another instance already owns the
    # port, fail without having made a single upstream request.
    try:
        server = UsageServer((args.host, args.port), UsageHandler)
    except OSError as e:
        print(f"Could not bind {args.host}:{args.port} -- {e}\n"
              "Another copy of this service is probably already running. "
              "Stop it first, or pass --port to use a different one.",
              file=sys.stderr)
        return 1

    # daemon=True so Ctrl-C actually exits: a non-daemon thread sitting in
    # time.sleep(60) would keep the process alive until that sleep finished.
    threading.Thread(target=poll_forever, args=(cache,), daemon=True).start()

    log(f"serving GET /usage on {args.host}:{args.port}  (Ctrl-C to stop)")

    # The line that saves a trip to ipconfig. This is the value to put in the
    # gadget's setup form, or in firmware/main/secrets.h -- and because DHCP
    # can move it, it is worth re-reading here rather than remembering what it
    # was last week.
    if args.host in ("0.0.0.0", ""):
        ip = lan_address()
        if ip:
            log(f"reachable on this network at {ip}:{args.port} "
                f"-- that is the address to give the gadget")
        else:
            log("could not work out this machine's LAN address; "
                "check with `ipconfig` (Windows) or `ip addr` (Linux)")
    else:
        log(f"bound to {args.host} only -- reachable from this machine, "
            f"not from the gadget" if args.host.startswith("127.")
            else f"bound to {args.host} only")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("stopping")
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
