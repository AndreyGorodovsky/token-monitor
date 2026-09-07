"""Throwaway stand-in for pc_service that always answers 503.

Serves exactly the shape ARCHITECTURE.md documents for "never polled
successfully yet", so the firmware's 503 branch can be exercised on demand
instead of waiting to catch the real service in that state.

Run it with the same python.exe pc_service uses -- the Windows firewall rule
is per-program, so a different interpreter is silently blocked and the chip
sees a timeout instead of a 503. Stop the real service first: see StubServer
below for what happens on Windows if you don't.
"""
import json
import sys
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




class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 30                # never wait forever on a parked keep-alive

    def do_GET(self):
        body = json.dumps({
            "stale": True,
            "error": "upstream request failed",
        }).encode("utf-8")
        self.send_response(503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("stub: " + (fmt % args), flush=True)


StubServer(("0.0.0.0", 8734), Stub).serve_forever()
