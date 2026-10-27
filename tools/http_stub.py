#!/usr/bin/env python3
"""Local stand-in for the Glu server on 127.0.0.1:18080 (same replies as EW2HTTPHandler in play_win.py)."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# The PvP/rank servers of Glu are gone. Answering them with a made-up body made the game run
# HttpMeta.decode on it (Base64 expected) and throw FormatException: 1-3 s freezes on every attempt.
# A 503 is what "server down" looks like to WWW, and the game has a path for that (isPvPServerOnLine,
# IDS_PVP_SERVER_DOWN) which does not decode anything.
OFFLINE_HOSTS = ("ew2rank", "bjapp-1.aws.glu.com")


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _offline(self):
        host = self.headers.get("Host", "")
        if any(h in host for h in OFFLINE_HOSTS) or self.path.startswith("/v1/private/pvp"):
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        return False

    def do_GET(self):
        if self._offline():
            return
        if "/version" in self.path:
            self._send(b"Version\nskip\nskip\nfalse\nAAAAAAAAAA\n0\n", "text/plain")
        else:
            self._send(b"{}", "application/json")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        if n > 0:
            self.rfile.read(n)
        if self._offline():
            return
        self._send(b'{"status":"ok"}', "application/json")


ThreadingHTTPServer(("127.0.0.1", 18080), H).serve_forever()
