#!/usr/bin/env python3
"""Minimal PC endpoint for ESP32-P4/C6 link validation and future frames."""

import argparse
import hashlib
import json
import logging
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from time import time_ns
from urllib.parse import urlsplit


class ReceiverHandler(BaseHTTPRequestHandler):
    server_version = "GenicanCameraReceiver/0.1"

    def _send_json(self, status, payload):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/health":
            self._send_json(200, {"ok": True, "service": "camera-receiver"})
            return
        self._send_json(404, {"ok": False, "error": "not_found"})

    def do_POST(self):
        path = urlsplit(self.path).path
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            self._send_json(411, {"ok": False, "error": "content_length_required"})
            return

        try:
            length = int(length_header)
        except ValueError:
            self._send_json(400, {"ok": False, "error": "invalid_content_length"})
            return

        max_body = self.server.max_body_bytes
        if length < 0 or length > max_body:
            self._send_json(413, {"ok": False, "error": "body_too_large", "max": max_body})
            return

        body = self.rfile.read(length)
        digest = hashlib.sha256(body).hexdigest()

        if path == "/echo":
            self._send_json(200, {"ok": True, "bytes": len(body), "sha256": digest})
            return

        if path == "/stream":
            output_dir = self.server.output_dir
            output_dir.mkdir(parents=True, exist_ok=True)
            extension = ".jpg" if self.headers.get_content_type() == "image/jpeg" else ".bin"
            output_path = output_dir / f"frame-{time_ns()}{extension}"
            output_path.write_bytes(body)
            self._send_json(200, {"ok": True, "bytes": len(body), "sha256": digest, "file": str(output_path)})
            return

        self._send_json(404, {"ok": False, "error": "not_found"})

    def log_message(self, fmt, *args):
        logging.info("%s - %s", self.address_string(), fmt % args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=8080, help="TCP port")
    parser.add_argument("--output", type=Path, default=Path("received_frames"), help="directory for /stream payloads")
    parser.add_argument("--max-body-bytes", type=int, default=8 * 1024 * 1024, help="maximum accepted POST body")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    server = ThreadingHTTPServer((args.host, args.port), ReceiverHandler)
    server.output_dir = args.output
    server.max_body_bytes = args.max_body_bytes
    logging.info("listening on http://%s:%d", args.host, args.port)
    logging.info("health endpoint: GET /health; echo endpoint: POST /echo; frame endpoint: POST /stream")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logging.info("stopping")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
