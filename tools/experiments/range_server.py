#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Minimal loopback HTTP file server for device experiments E2 and E7.

Python 3 stdlib only; run with `python3 -I`. Not the product's fetcher (RES-17): it only mimics the
response semantics GStreamer sees from it, so pipelines can be measured before the app exists.

    python3 -I range_server.py --root DIR [--no-range] [--rate-kbps N] [--rtt-ms N] [--port-file F]

* Binds 127.0.0.1 on an ephemeral port. Prints `range-server: port=<p> token=<t>` and flushes.
* Serves DIR/<name> at /<token>/<name>. A path without the token gets 403.
* Range: `206` with `Content-Range`, `416 bytes */<total>` past the end; `200` without Range.
* --no-range ignores Range and omits Accept-Ranges (the non-byte-seekable upstream of PLY-04).
* --rate-kbps caps the per-response send rate; --rtt-ms delays each response start.

Importable: `start(root, ...)` returns (server, port, token) for in-process use (E2).
"""
import argparse
import http.server
import os
import re
import secrets
import socketserver
import sys
import threading
import time

RANGE = re.compile(r"^bytes=(\d*)-(\d*)$")


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def _handler(root, token, ranged, rate_kbps, rtt_ms):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("range-server: %s %s\n" % (self.address_string(), fmt % args))

        def _fail(self, code, extra=None):
            self.send_response(code)
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _serve(self, body):
            parts = self.path.split("/")
            if len(parts) != 3 or parts[1] != token or not parts[2] or parts[2].startswith("."):
                return self._fail(403)
            path = os.path.join(root, parts[2])
            if not os.path.isfile(path):
                return self._fail(404)
            total = os.path.getsize(path)
            first, last, code = 0, total - 1, 200
            header = self.headers.get("Range")
            if ranged and header:
                match = RANGE.match(header.strip())
                if not match or (not match.group(1) and not match.group(2)):
                    return self._fail(416, {"Content-Range": "bytes */%d" % total})
                if match.group(1):
                    first = int(match.group(1))
                    last = min(int(match.group(2)), total - 1) if match.group(2) else total - 1
                else:
                    first = max(0, total - int(match.group(2)))
                if first >= total or first > last:
                    return self._fail(416, {"Content-Range": "bytes */%d" % total})
                code = 206
            if rtt_ms:
                time.sleep(rtt_ms / 1000.0)
            self.send_response(code)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(last - first + 1))
            if ranged:
                self.send_header("Accept-Ranges", "bytes")
            if code == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (first, last, total))
            self.end_headers()
            if not body:
                return
            remaining = last - first + 1
            chunk = 65536
            with open(path, "rb") as handle:
                handle.seek(first)
                started = time.monotonic()
                sent = 0
                while remaining > 0:
                    data = handle.read(min(chunk, remaining))
                    if not data:
                        break
                    try:
                        self.wfile.write(data)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    sent += len(data)
                    remaining -= len(data)
                    if rate_kbps:
                        ahead = sent * 8 / 1000.0 / rate_kbps - (time.monotonic() - started)
                        if ahead > 0:
                            time.sleep(ahead)

        def do_GET(self):
            self._serve(True)

        def do_HEAD(self):
            self._serve(False)

    return Handler


def start(root, ranged=True, rate_kbps=0, rtt_ms=0):
    """Starts the server on a daemon thread; returns (server, port, token)."""
    token = secrets.token_urlsafe(16)
    server = _Server(("127.0.0.1", 0), _handler(os.path.abspath(root), token, ranged, rate_kbps, rtt_ms))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, server.server_address[1], token


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--no-range", action="store_true")
    parser.add_argument("--rate-kbps", type=int, default=0)
    parser.add_argument("--rtt-ms", type=int, default=0)
    parser.add_argument("--port-file")
    args = parser.parse_args()
    server, port, token = start(args.root, not args.no_range, args.rate_kbps, args.rtt_ms)
    line = "range-server: port=%d token=%s" % (port, token)
    if args.port_file:
        with open(args.port_file + ".tmp", "w") as handle:
            handle.write("%d %s\n" % (port, token))
        os.rename(args.port_file + ".tmp", args.port_file)
    print(line, flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
