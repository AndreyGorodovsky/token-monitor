"""Stage 1: prove we can read the usage endpoint.

Reads Claude Code's own OAuth token from its credential file, makes ONE
request to Anthropic's (undocumented) usage endpoint, and pretty-prints the
parsed JSON.

Security rules this file obeys (see ../CLAUDE.md and ../SECRETS.md):
  - the token is read from disk at runtime, never copied into this project
  - the token is sent to api.anthropic.com and nowhere else
  - the token is never printed, logged, or written to a file

This file is deliberately runnable on its own. service.py imports
read_access_token() and fetch_usage() from here rather than duplicating them,
so when the display shows something wrong, running this script directly tells
you within seconds whether the problem is upstream (Anthropic, the token) or
downstream (the service, the network, the firmware).

READ IT IN THIS ORDER: read_access_token (get the credential), fetch_usage
(use it), then main() (print what came back).

Run:  python fetch_usage.py
"""

import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

# Windows (%USERPROFILE%) and Linux (~) both keep Claude Code's token in this
# JSON file, and Path.home() resolves correctly on both. macOS is deliberately
# NOT claimed here -- see the darwin branch in read_access_token().
CREDENTIALS_PATH = pathlib.Path.home() / ".claude" / ".credentials.json"

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
ANTHROPIC_BETA = "oauth-2025-04-20"
TIMEOUT_SECONDS = 15


class RateLimited(RuntimeError):
    """The endpoint returned 429.

    Distinct from other failures because the correct response is different:
    a network blip should be retried quickly, but a 429 means the server
    explicitly asked us to slow down, and retrying on the normal schedule
    makes it worse. Carries the server's Retry-After hint when it sends one.
    """

    def __init__(self, message, retry_after=None):
        super().__init__(message)
        self.retry_after = retry_after


# --- data source layer -------------------------------------------------
# Kept separate from everything else on purpose: if this ever moves off
# "the PC reads its own credential file", only this function changes.

def read_access_token(announce=False):
    """Return the OAuth access token, or raise RuntimeError explaining why not.

    Re-reads the file on every call rather than caching: Claude Code refreshes
    this token periodically, and a long-running service must pick up the new
    value instead of holding a stale one until it 401s.

    `announce=True` prints how long the token remains valid. Only main() sets
    it -- the service polls once a minute and would otherwise emit that line
    1,440 times a day. Note it prints the token's *lifetime*, never the token.

    Every failure below raises RuntimeError with a sentence explaining what to
    do about it, rather than letting a KeyError or TypeError escape. This is
    the layer most likely to break when something outside this project changes
    (Claude Code moving or renaming its credential storage), so it is the
    layer that most needs to fail legibly.
    """
    try:
        raw = CREDENTIALS_PATH.read_text(encoding="utf-8")
    except FileNotFoundError:
        if sys.platform == "darwin":
            raise RuntimeError(
                "macOS is not supported yet. Claude Code stores its token in the "
                "system Keychain there, not in a JSON file, so this file-reading "
                "path finds nothing. Supporting macOS means reading the Keychain "
                "instead -- untested here, so it is left unimplemented rather "
                "than guessed at."
            )
        raise RuntimeError(
            f"No credential file at {CREDENTIALS_PATH}.\n"
            "Is Claude Code installed and logged in on this machine?"
        )
    except OSError as e:
        raise RuntimeError(f"Could not read {CREDENTIALS_PATH}: {e}")

    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"Credential file is not valid JSON: {e}")

    oauth = data.get("claudeAiOauth")
    if not isinstance(oauth, dict):
        raise RuntimeError(
            "Credential file has no 'claudeAiOauth' object -- Claude Code may "
            "have changed its storage format."
        )

    token = oauth.get("accessToken")
    if not isinstance(token, str) or not token:
        raise RuntimeError("Credential file has no usable 'accessToken'.")

    # expiresAt is a millisecond epoch. Warn but don't refuse -- a stale
    # token gives a 401 below, which is a clearer signal than a guess here.
    expires_at_ms = oauth.get("expiresAt")
    if isinstance(expires_at_ms, int):
        seconds_left = expires_at_ms / 1000 - time.time()
        if seconds_left < 0:
            print(f"WARNING: token expired {-seconds_left / 3600:.1f}h ago; "
                  "run any Claude Code command to refresh it.", file=sys.stderr)
        elif announce:
            print(f"[token valid for another {seconds_left / 3600:.1f}h]")

    return token


def fetch_usage(token):
    """GET the usage endpoint. Returns parsed JSON, or raises RuntimeError.

    The only place in this project the token is used, and it goes into an
    Authorization header on a URL hard-coded to api.anthropic.com -- not into
    a URL, a log line, or a variable that outlives this call.

    urllib is used rather than `requests` so the whole service stays pure
    standard library: no pip install, no virtualenv. That is not just tidiness
    -- the Windows firewall rule that lets the ESP32 reach this service is
    bound to a specific python.exe path, so introducing a venv would silently
    break reachability. See ../pc_service/README.md.
    """
    request = urllib.request.Request(
        USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": ANTHROPIC_BETA,
            "Accept": "application/json",
        },
        method="GET",
    )

    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            body = response.read().decode("utf-8", errors="replace")
            status = response.status
    # Clause order matters: HTTPError is a subclass of URLError, so catching
    # URLError first would swallow every HTTP status response and report it as
    # an unreachable host.
    except urllib.error.HTTPError as e:
        # HTTPError is raised for any non-2xx status, and it doubles as a
        # file-like object holding the response body. That body is usually the
        # most useful clue about WHY, so read it -- capped at 500 characters,
        # since it goes into an exception message that gets logged.
        #
        # This detail stays on the PC. service.py deliberately does not put it
        # in the LAN response: a 401/403 body can carry request IDs and account
        # detail, and that endpoint is unauthenticated.
        detail = e.read().decode("utf-8", errors="replace")[:500]

        if e.code == 429:
            # Keep this one short: it is logged repeatedly while backing off,
            # and the full JSON body says nothing the status code doesn't.
            retry_after = None
            header = e.headers.get("Retry-After") if e.headers else None
            if header:
                try:
                    retry_after = max(0, int(float(header)))
                except (TypeError, ValueError):
                    retry_after = None      # HTTP-date form; fall back to backoff
            raise RateLimited("HTTP 429 rate limited by the usage endpoint.",
                              retry_after=retry_after)

        raise RuntimeError(f"HTTP {e.code} {e.reason} from the usage endpoint.\n{detail}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"Could not reach {USAGE_URL}: {e.reason}")
    except TimeoutError:
        raise RuntimeError(f"Timed out after {TIMEOUT_SECONDS}s reaching {USAGE_URL}")

    # Defensive: urlopen already raises HTTPError for non-2xx, so reaching
    # here with a non-200 means a 2xx we did not expect (204, 206...). Treat
    # it as a failure rather than trying to parse it as usage data.
    if status != 200:
        raise RuntimeError(f"Unexpected status {status} from the usage endpoint.")

    try:
        return json.loads(body)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"Response was not valid JSON: {e}\nFirst 300 chars: {body[:300]}")


def main():
    """Stage 1's whole purpose: print what the endpoint returns, so it can be
    eyeballed against what /status shows inside Claude Code.

    Returns a process exit code (0 ok, 1 failed) rather than calling sys.exit
    itself, so the failure path stays testable and the __main__ block below is
    the only thing that actually exits.
    """
    try:
        token = read_access_token(announce=True)
        usage = fetch_usage(token)
    except RuntimeError as e:
        print(f"FAILED: {e}", file=sys.stderr)
        return 1

    print("\n--- raw response ---")
    print(json.dumps(usage, indent=2, sort_keys=True))

    # A small summary in the shape the firmware will eventually care about,
    # so it's easy to eyeball against /status in Claude Code.
    print("\n--- what /status should agree with ---")
    if not isinstance(usage, dict):
        print("(response was not a JSON object -- nothing to summarize)")
        return 0
    for key in ("five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"):
        window = usage.get(key)
        if not isinstance(window, dict):
            print(f"{key:18} (absent or null)")
            continue
        print(f"{key:18} {window.get('utilization')}% used, resets {window.get('resets_at')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
