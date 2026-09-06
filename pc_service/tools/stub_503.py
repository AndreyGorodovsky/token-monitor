"""Throwaway stand-in for pc_service that always answers 503.

Serves exactly the shape ARCHITECTURE.md documents for "never polled
successfully yet", so the firmware's 503 branch can be exercised on demand
instead of waiting to catch the real service in that state.

Run it with the same python.exe pc_service uses -- the Windows firewall rule
is per-program, so a different interpreter is silently blocked and the chip
sees a timeout instead of a 503.
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

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


HTTPServer(("0.0.0.0", 8734), Stub).serve_forever()
