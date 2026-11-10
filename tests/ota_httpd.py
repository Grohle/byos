#!/usr/bin/env python3
"""Small HTTP server for tests/ota_test.sh: GitHub-like release URLs with Range support.

  ota_httpd.py ROOT PORTFILE LOGFILE

  GET /latest/download/<f>  -> 302 to /dl/<f>   (like github.com/.../releases/latest/download)
  GET /dl/<f>               -> ROOT/<f>; "Range: bytes=N-" -> 206 (416 if N >= size)
  ROOT/.cut (a number N)    -> the next full GET of a .tar sends N bytes of the body and closes
                               the connection (a dropped download); the file is then removed
  ROOT/.slow (ms per 64 KB) -> slows .tar bodies down (to pause a download from the test)
Every request is appended to LOGFILE as "METHOD PATH RANGE STATUS".
"""
import http.server
import os
import sys
import time

ROOT, PORTFILE, LOG = sys.argv[1], sys.argv[2], sys.argv[3]


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _log(self, status):
        with open(LOG, "a") as f:
            f.write(f"{self.command} {self.path} {self.headers.get('Range', '-')} {status}\n")

    def do_GET(self):
        p = self.path.split("?")[0]
        if p.startswith("/latest/download/"):
            self.send_response(302)
            self.send_header("Location", "/dl/" + p[len("/latest/download/"):])
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self._log(302)
            return
        name = p[len("/dl/"):] if p.startswith("/dl/") else ""
        fp = os.path.join(ROOT, name)
        if not name or "/" in name or name.startswith(".") or not os.path.isfile(fp):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self._log(404)
            return
        size = os.path.getsize(fp)
        start, status = 0, 200
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes=") and rng.endswith("-"):
            start = int(rng[6:-1])
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self._log(416)
                return
            status = 206
        cut = None
        cutf = os.path.join(ROOT, ".cut")
        if name.endswith(".tar") and status == 200 and os.path.isfile(cutf):
            with open(cutf) as f:
                cut = int(f.read().strip())
            os.remove(cutf)
        slow = 0
        if name.endswith(".tar") and os.path.isfile(os.path.join(ROOT, ".slow")):
            with open(os.path.join(ROOT, ".slow")) as f:
                slow = int(f.read().strip()) / 1000.0
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size - start))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
        self.send_header("Connection", "close")
        self.end_headers()
        self._log(f"{status}{' cut' if cut is not None else ''}")
        sent = 0
        with open(fp, "rb") as f:
            f.seek(start)
            while True:
                b = f.read(65536)
                if not b:
                    break
                if cut is not None and sent + len(b) > cut:
                    b = b[:cut - sent]
                    self.wfile.write(b)
                    self.wfile.flush()
                    self.close_connection = True
                    return
                try:
                    self.wfile.write(b)
                except (BrokenPipeError, ConnectionResetError):
                    return
                sent += len(b)
                if slow:
                    time.sleep(slow)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
with open(PORTFILE + ".tmp", "w") as f:
    f.write(str(srv.server_address[1]))
os.replace(PORTFILE + ".tmp", PORTFILE)
srv.serve_forever()
