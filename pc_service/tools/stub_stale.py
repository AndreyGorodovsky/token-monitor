"""Throwaway stand-in for pc_service that serves data of a chosen age.

The firmware escalates in three steps as data gets older: no badge, then an
amber STALE badge, then the numbers themselves go grey because they have
stopped being a claim about the present. The thresholds are 10 and 30 minutes
(see STALE_BADGE_AGE_S / STALE_DEAD_AGE_S in the firmware), which makes those
last two branches awkward to reach honestly -- you would have to break the real
service and then wait half an hour to see whether the screen did the right
thing.

So this serves a perfectly well-formed 200 whose `updated_epoch` is however far
in the past you ask for. Everything else about the payload is real-shaped, so
the firmware takes exactly the code path it would take against a genuinely
stalled service.

    python tools/stub_stale.py 900     # 15 min old -> amber STALE badge
    python tools/stub_stale.py 2400    # 40 min old -> numbers go grey
    python tools/stub_stale.py 60 --flag   # fresh, but flagged stale by the PC

The last form is worth knowing about: the `stale` flag and the age are
independent signals, and the flag alone has to be enough to raise the badge --
that is the case where pc_service is alive and its own upstream call failed.

Run it with the same python.exe pc_service uses -- the Windows firewall rule
is per-program, so a different interpreter is silently blocked and the chip
sees a timeout instead of a reply. Stop the real service first: both bind 8734,
and StubServer below is what makes the second one fail loudly rather than
quietly sharing the port with the first.
"""
import argparse
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class StubServer(ThreadingHTTPServer):
    """Same two guards `service.py` needs, and for the same reasons.

    **Refuses to start beside a running instance.** Python sets SO_REUSEADDR by
    default, which means different things per OS: on Linux it only allows
    rebinding a port stuck in TIME_WAIT, but on Windows it lets a second
    process bind a port that is *already actively listening*. Left on, this
    stub would happily bind alongside a running `service.py` and Windows would
    split incoming connections between the two nondeterministically -- so the
    chip would show real fresh data on some polls and this stub's data on
    others, which reads as the firmware flapping rather than as the obvious
    mistake it is. Failing loudly at bind time is much easier to diagnose.

    **Threaded, with a timeout.** `protocol_version = "HTTP/1.1"` means
    keep-alive by default, and on a single-threaded server one parked
    connection blocks every other client. The natural way to check this stub is
    serving the age you asked for is to open it in a browser -- and that tab's
    persistent connection would then hold the only thread indefinitely, the
    ESP32's next poll would never be accepted, and the screen would show
    NO LINK: the exact opposite of the branch this tool exists to exercise.
    """

    allow_reuse_address = sys.platform != "win32"


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("age", type=int, nargs="?", default=900,
                    help="how many seconds old the data should claim to be")
parser.add_argument("--flag", action="store_true",
                    help="also set the stale flag, as pc_service does when its "
                         "own upstream call fails")
args = parser.parse_args()


class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 30                # never wait forever on a parked keep-alive

    def do_GET(self):
        now = int(time.time())
        updated = now - args.age

        # Percentages high enough to be colour-banded amber/red when fresh, so
        # the grey-out at 30 minutes is unmistakable rather than a subtle shift
        # away from a colour that was already dull.
        body = json.dumps({
            "five_hour_pct": 73,
            "five_hour_resets_at": time.strftime("%H:%M",
                                                 time.localtime(now + 3600)),
            "five_hour_resets_epoch": now + 3600,
            "seven_day_pct": 88,
            "seven_day_resets_at": time.strftime("%a %H:%M",
                                                 time.localtime(now + 86400)),
            "seven_day_resets_epoch": now + 86400,
            "updated_at": time.strftime("%H:%M", time.localtime(updated)),
            "updated_epoch": updated,
            "now_epoch": now,
            "stale": args.flag,
        }).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args_):
        print("stub: " + (fmt % args_), flush=True)


print("serving %ds-old data on 0.0.0.0:8734 (stale flag: %s)"
      % (args.age, args.flag), flush=True)
StubServer(("0.0.0.0", 8734), Stub).serve_forever()
