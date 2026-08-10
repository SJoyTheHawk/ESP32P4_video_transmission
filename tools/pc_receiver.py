#!/usr/bin/env python3
"""Receive length-framed JPEG images over one persistent TCP connection."""

import argparse
import socket
import struct
from time import time_ns


HEADER = struct.Struct("!4sIHHI")
MAGIC = b"JPG0"


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


def receive_frames(connection, max_jpeg_bytes):
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
        valid = jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
        latest_jpeg = jpeg
        print(
            f"jpeg sequence={sequence} width={width} height={height} "
            f"bytes={jpeg_size} valid={'yes' if valid else 'no'} "
            f"received_ns={time_ns()}",
            flush=True,
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="bind address")
    parser.add_argument("--port", type=int, default=5001, help="TCP port")
    parser.add_argument(
        "--max-jpeg-bytes",
        type=int,
        default=2 * 1024 * 1024,
        help="maximum accepted JPEG payload",
    )
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(1)
        print(f"listening on {args.host}:{args.port}", flush=True)

        connection, address = server.accept()
        with connection:
            print(f"sender connected from {address[0]}:{address[1]}", flush=True)
            receive_frames(connection, args.max_jpeg_bytes)


if __name__ == "__main__":
    main()
