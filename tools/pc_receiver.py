#!/usr/bin/env python3
"""Receive length-framed JPEGs and expose the latest frame as an MJPEG stream."""

import argparse
import multiprocessing
import socket
import struct
import threading
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Full
from time import time_ns


HEADER = struct.Struct("!4sIHHI")
MAGIC = b"JPG0"
MJPEG_BOUNDARY = "frame"
PREVIEW_QUEUE_DEPTH = 2
DEFAULT_TCP_RECEIVE_BUFFER = 2 * 1024 * 1024


@dataclass(frozen=True)
class FrameSnapshot:
    version: int
    sequence: int
    width: int
    height: int
    jpeg: bytes


class LatestFrame:
    def __init__(self):
        self._condition = threading.Condition()
        self._snapshot = None
        self._version = 0

    def publish(self, sequence, width, height, jpeg):
        with self._condition:
            self._version += 1
            self._snapshot = FrameSnapshot(
                self._version, sequence, width, height, jpeg
            )
            self._condition.notify_all()

    def wait_after(self, version):
        with self._condition:
            self._condition.wait_for(
                lambda: self._snapshot is not None and self._version > version
            )
            return self._snapshot


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            if data:
                raise ConnectionError(
                    f"connection closed after {len(data)} of {size} bytes"
                )
            return None
        data.extend(chunk)
    return bytes(data)


def publish_preview_frame(frame_queue, sequence, width, height, jpeg):
    try:
        frame_queue.put_nowait((sequence, width, height, jpeg))
    except Full:
        # Preview is best-effort. TCP ingestion must never wait for a browser.
        pass


def receive_frames(connection, max_jpeg_bytes, frame_queue):
    latest_jpeg = None
    while True:
        header = receive_exact(connection, HEADER.size)
        if header is None:
            print("sender disconnected")
            return latest_jpeg

        magic, sequence, width, height, jpeg_size = HEADER.unpack(header)
        if magic != MAGIC:
            raise ValueError(f"invalid magic {magic!r}")
        if jpeg_size < 4 or jpeg_size > max_jpeg_bytes:
            raise ValueError(f"invalid JPEG length {jpeg_size}")

        jpeg = receive_exact(connection, jpeg_size)
        if jpeg is None:
            raise ConnectionError("connection closed before JPEG payload")
        valid = jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
        latest_jpeg = jpeg
        if valid:
            publish_preview_frame(frame_queue, sequence, width, height, jpeg)
        print(
            f"jpeg sequence={sequence} width={width} height={height} "
            f"bytes={jpeg_size} valid={'yes' if valid else 'no'} "
            f"received_ns={time_ns()}",
            flush=True,
        )


def receive_from_sender(server, max_jpeg_bytes, frame_queue, receive_buffer):
    connection, address = server.accept()
    with connection:
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        actual_receive_buffer = connection.getsockopt(
            socket.SOL_SOCKET, socket.SO_RCVBUF
        )
        print(
            f"sender connected from {address[0]}:{address[1]} "
            f"receive_buffer={actual_receive_buffer}",
            flush=True,
        )
        receive_frames(connection, max_jpeg_bytes, frame_queue)


class PreviewHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.partition("?")[0]
        if path == "/":
            self.send_response(302)
            self.send_header("Location", "/stream.mjpg")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path != "/stream.mjpg":
            self.send_error(404)
            return

        self.send_response(200)
        self.send_header(
            "Content-Type",
            f"multipart/x-mixed-replace; boundary={MJPEG_BOUNDARY}",
        )
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        version = 0
        self.connection.settimeout(2.0)
        try:
            while True:
                frame = self.server.latest_frame.wait_after(version)
                version = frame.version
                part_header = (
                    f"--{MJPEG_BOUNDARY}\r\n"
                    "Content-Type: image/jpeg\r\n"
                    f"Content-Length: {len(frame.jpeg)}\r\n"
                    f"X-Sequence: {frame.sequence}\r\n"
                    f"X-Width: {frame.width}\r\n"
                    f"X-Height: {frame.height}\r\n\r\n"
                ).encode("ascii")
                self.wfile.write(part_header)
                self.wfile.write(frame.jpeg)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except OSError:
            return

    def log_message(self, _format, *_args):
        return


class PreviewServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address, latest_frame):
        super().__init__(server_address, PreviewHandler)
        self.latest_frame = latest_frame


def receive_preview_frames(frame_queue, latest_frame):
    while True:
        sequence, width, height, jpeg = frame_queue.get()
        latest_frame.publish(sequence, width, height, jpeg)


def run_preview_server(http_host, http_port, frame_queue):
    latest_frame = LatestFrame()
    receiver_thread = threading.Thread(
        target=receive_preview_frames,
        args=(frame_queue, latest_frame),
        daemon=True,
        name="preview-frame-receiver",
    )
    receiver_thread.start()

    with PreviewServer((http_host, http_port), latest_frame) as preview_server:
        actual_port = preview_server.server_address[1]
        print(
            f"MJPEG preview listening on http://{http_host}:{actual_port}/",
            flush=True,
        )
        preview_server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=5001, help="TCP port")
    parser.add_argument(
        "--http-host",
        help="HTTP preview bind address (defaults to --host)",
    )
    parser.add_argument(
        "--http-port",
        type=int,
        default=8080,
        help="HTTP preview port",
    )
    parser.add_argument(
        "--max-jpeg-bytes",
        type=int,
        default=2 * 1024 * 1024,
        help="maximum accepted JPEG payload",
    )
    parser.add_argument(
        "--tcp-receive-buffer",
        type=int,
        default=DEFAULT_TCP_RECEIVE_BUFFER,
        help="requested TCP receive-buffer size",
    )
    args = parser.parse_args()
    http_host = args.http_host if args.http_host is not None else args.host
    frame_queue = multiprocessing.Queue(maxsize=PREVIEW_QUEUE_DEPTH)
    preview_process = multiprocessing.Process(
        target=run_preview_server,
        args=(http_host, args.http_port, frame_queue),
        daemon=True,
        name="mjpeg-preview",
    )
    preview_process.start()

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp_server:
            tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            tcp_server.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_RCVBUF,
                args.tcp_receive_buffer,
            )
            tcp_server.bind((args.host, args.port))
            tcp_server.listen(1)

            tcp_port = tcp_server.getsockname()[1]
            print(f"JPEG receiver listening on {args.host}:{tcp_port}", flush=True)
            receive_from_sender(
                tcp_server,
                args.max_jpeg_bytes,
                frame_queue,
                args.tcp_receive_buffer,
            )
    except KeyboardInterrupt:
        print("stopping", flush=True)
    finally:
        if preview_process.is_alive():
            preview_process.terminate()
        preview_process.join(timeout=2.0)
        frame_queue.cancel_join_thread()


if __name__ == "__main__":
    main()
