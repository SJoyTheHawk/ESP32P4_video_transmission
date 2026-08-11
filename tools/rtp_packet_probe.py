#!/usr/bin/env python3
"""Validate the ESP32 RFC 2435 RTP/JPEG packets without decoding them."""

import argparse
import random
import socket
import struct
from urllib.parse import urlparse


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "url", nargs="?", default="rtsp://192.168.1.140:554/"
    )
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def open_udp_pair(timeout: float):
    for _ in range(100):
        port = random.randrange(20_000, 60_000, 2)
        rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rtp.bind(("0.0.0.0", port))
            rtcp.bind(("0.0.0.0", port + 1))
            rtp.settimeout(timeout)
            rtcp.settimeout(timeout)
            return rtp, rtcp, port
        except OSError:
            rtp.close()
            rtcp.close()
    raise RuntimeError("could not allocate adjacent RTP/RTCP client ports")


def receive_response(control: socket.socket):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = control.recv(4096)
        if not chunk:
            raise RuntimeError("RTSP control socket closed")
        data.extend(chunk)
    header, body = bytes(data).split(b"\r\n\r\n", 1)
    lines = header.decode("ascii").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    content_length = int(headers.get("content-length", "0"))
    while len(body) < content_length:
        body += control.recv(content_length - len(body))
    if status != 200:
        raise RuntimeError(f"RTSP request failed: {lines[0]}")
    return headers, body[:content_length]


def send_request(control, method, url, cseq, headers=None):
    fields = {"CSeq": str(cseq)}
    if headers:
        fields.update(headers)
    request = f"{method} {url} RTSP/1.0\r\n"
    request += "".join(f"{key}: {value}\r\n" for key, value in fields.items())
    control.sendall((request + "\r\n").encode("ascii"))
    return receive_response(control)


def timestamp_is_forward(previous: int, current: int) -> bool:
    difference = (current - previous) & 0xFFFFFFFF
    return 0 < difference < 0x80000000


def probe(args: argparse.Namespace) -> None:
    parsed = urlparse(args.url)
    if parsed.scheme != "rtsp" or not parsed.hostname:
        raise ValueError("URL must use rtsp:// and include a host")
    host = parsed.hostname
    port = parsed.port or 554
    base_url = f"rtsp://{host}:{port}/"
    video_url = f"{base_url}video"

    rtp, rtcp, client_port = open_udp_pair(args.timeout)
    control = socket.create_connection((host, port), timeout=args.timeout)
    control.settimeout(args.timeout)
    session = None
    cseq = 1

    try:
        send_request(control, "OPTIONS", base_url, cseq)
        cseq += 1
        _, sdp = send_request(
            control, "DESCRIBE", base_url, cseq, {"Accept": "application/sdp"}
        )
        cseq += 1
        if b"m=video 0 RTP/AVP 26" not in sdp or b"a=control:video" not in sdp:
            raise RuntimeError("SDP does not advertise static RTP/JPEG payload 26")

        setup_headers, _ = send_request(
            control,
            "SETUP",
            video_url,
            cseq,
            {"Transport": f"RTP/AVP;unicast;client_port={client_port}-{client_port + 1}"},
        )
        cseq += 1
        session = setup_headers.get("session", "").split(";", 1)[0]
        transport = setup_headers.get("transport", "")
        if not session or "server_port=5430-5431" not in transport:
            raise RuntimeError("SETUP did not negotiate server ports 5430-5431")

        send_request(
            control, "PLAY", base_url, cseq, {"Session": session}
        )
        cseq += 1

        previous_sequence = None
        previous_timestamp = None
        expected_offset = 0
        packet_count = 0
        frame_packets = 0
        frame_bytes = 0
        frames = 0
        frame_was_marked = False
        maximum_datagram = 0

        while frames < args.frames:
            packet, source = rtp.recvfrom(2048)
            if source[0] != host or source[1] != 5430:
                raise RuntimeError(f"unexpected RTP source {source}")
            if len(packet) > 1400 or len(packet) < 20:
                raise RuntimeError(f"invalid RTP datagram size {len(packet)}")
            maximum_datagram = max(maximum_datagram, len(packet))

            first, payload_type, sequence, timestamp = struct.unpack(
                "!BBHI", packet[:8]
            )
            marker = bool(payload_type & 0x80)
            if first != 0x80 or (payload_type & 0x7F) != 26:
                raise RuntimeError("invalid RTP version or payload type")
            expected_sequence = None
            if previous_sequence is not None:
                expected_sequence = (previous_sequence + 1) & 0xFFFF
            if expected_sequence is not None and sequence != expected_sequence:
                raise RuntimeError(
                    f"RTP sequence gap: {previous_sequence} then {sequence}"
                )
            previous_sequence = sequence

            fragment_offset = int.from_bytes(packet[13:16], "big")
            if packet[16] != 0 or packet[17] != 255:
                raise RuntimeError("RTP/JPEG type is not 0 or Q is not 255")
            if packet[18] != 100 or packet[19] != 100:
                raise RuntimeError("RTP/JPEG dimensions are not 800x800")

            if previous_timestamp is None or timestamp != previous_timestamp:
                if previous_timestamp is not None:
                    if not frame_was_marked:
                        raise RuntimeError("previous frame did not end with a marker")
                    if not timestamp_is_forward(previous_timestamp, timestamp):
                        raise RuntimeError("RTP timestamp did not advance")
                if fragment_offset != 0:
                    raise RuntimeError("new frame does not begin at fragment offset zero")
                previous_timestamp = timestamp
                expected_offset = 0
                frame_packets = 0
                frame_bytes = 0
                frame_was_marked = False

            if frame_was_marked:
                raise RuntimeError("packet followed the marker with the same timestamp")
            if fragment_offset != expected_offset:
                raise RuntimeError(
                    f"fragment offset {fragment_offset} expected {expected_offset}"
                )

            payload_start = 20
            if fragment_offset == 0:
                if len(packet) < 152 or packet[20:22] != b"\x00\x00":
                    raise RuntimeError("first packet lacks an 8-bit quantization header")
                if int.from_bytes(packet[22:24], "big") != 128:
                    raise RuntimeError("first packet lacks two quantization tables")
                if not any(packet[24:152]):
                    raise RuntimeError("quantization tables are empty")
                payload_start = 152

            scan_bytes = len(packet) - payload_start
            if scan_bytes <= 0:
                raise RuntimeError("RTP/JPEG fragment has no entropy data")
            expected_offset += scan_bytes
            frame_packets += 1
            frame_bytes += scan_bytes
            packet_count += 1

            if marker:
                frame_was_marked = True
                frames += 1
                print(
                    f"rtp_frame={frames} timestamp={timestamp} "
                    f"packets={frame_packets} scan_bytes={frame_bytes} marker=yes"
                )

        print(
            f"summary frames={frames} packets={packet_count} "
            f"max_datagram={maximum_datagram} "
            "sequence=monotonic offsets=valid marker=final-only "
            "quantization_tables=valid result=pass"
        )
    finally:
        if session:
            try:
                send_request(
                    control, "TEARDOWN", base_url, cseq, {"Session": session}
                )
            except (OSError, RuntimeError):
                pass
        control.close()
        rtp.close()
        rtcp.close()


def main() -> int:
    args = parse_args()
    if args.frames <= 0 or args.timeout <= 0:
        print("frames and timeout must be positive")
        return 2
    try:
        probe(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"probe failed: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
