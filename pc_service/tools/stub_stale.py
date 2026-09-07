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
sees a timeout instead of a reply. Stop the real service first; both bind 8734,
and the second one to start fails loudly rather than sharing the port.
"""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("age", type=int, nargs="?", default=900,
                    help="how many seconds old the data should claim to be")
parser.add_argument("--flag", action="store_true",
                    help="also set the stale flag, as pc_service does when its "
                         "own upstream call fails")
args = parser.parse_args()


class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

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
HTTPServer(("0.0.0.0", 8734), Stub).serve_forever()
