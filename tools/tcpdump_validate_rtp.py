#!/usr/bin/env python3
"""Capture RTP/JPEG traffic with tcpdump and validate IPv4 fragmentation."""

import argparse
from dataclasses import dataclass
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time


IP_HEADER_RE = re.compile(
    r"\bIP\s+\(.*?\boffset\s+(?P<offset>\d+),\s+"
    r"flags\s+\[(?P<flags>[^]]*)\].*?\blength\s+(?P<length>\d+)\)"
)
SOURCE_RE = re.compile(
    r"\b(?P<source>\d{1,3}(?:\.\d{1,3}){3})\.5430\s+>"
)
UDP_LENGTH_RE = re.compile(r"\bUDP,\s+length\s+(?P<length>\d+)")


@dataclass
class Packet:
    source: str
    ipv4_length: int
    udp_length: int
    fragment_offset: int
    flags: str

    @property
    def fragmented(self) -> bool:
        return self.fragment_offset != 0 or "MF" in self.flags


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default="192.168.2.57",
        help="ESP32 source address to capture (default: %(default)s)",
    )
    parser.add_argument(
        "--interface",
        help="capture interface; defaults to the interface used by the default route",
    )
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("tools/rtp_capture.pcap"),
        help="pcap output path (default: %(default)s)",
    )
    parser.add_argument(
        "--tcpdump",
        default="tcpdump",
        help="tcpdump executable (default: %(default)s)",
    )
    return parser.parse_args()


def default_interface() -> str:
    route = shutil.which("route")
    if route:
        result = subprocess.run(
            [route, "-n", "get", "default"],
            capture_output=True,
            text=True,
            check=False,
        )
        match = re.search(r"^interface:\s*(\S+)", result.stdout, re.MULTILINE)
        if match:
            return match.group(1)
    raise RuntimeError("could not determine the default network interface; use --interface")


def stop_tcpdump(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def capture(args: argparse.Namespace, interface: str) -> int:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    capture_filter = f"host {args.host} and udp port 5430"
    command = [
        args.tcpdump,
        "-i",
        interface,
        "-nn",
        "-s",
        "0",
        "-w",
        str(args.output),
        capture_filter,
    ]
    print("capture_command=" + " ".join(command), flush=True)
    print("start an RTSP viewer or receiver while capture is running", flush=True)
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as error:
        raise RuntimeError(f"tcpdump executable not found: {args.tcpdump}") from error
    try:
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            time.sleep(0.2)
    except KeyboardInterrupt:
        print("capture interrupted", flush=True)
    finally:
        stop_tcpdump(process)

    stderr = process.stderr.read() if process.stderr else ""
    if process.returncode not in (0, 2):
        detail = stderr.strip() or f"tcpdump exited with status {process.returncode}"
        if "Permission denied" in detail or "BPF" in detail:
            detail += "; retry with sudo"
        raise RuntimeError(detail)
    if stderr.strip() and process.returncode != 0:
        print(stderr.strip(), file=sys.stderr)
    if not args.output.exists() or args.output.stat().st_size == 0:
        raise RuntimeError(f"tcpdump did not create a capture: {args.output}")
    return process.returncode or 0


def parse_tcpdump(args: argparse.Namespace) -> list[Packet]:
    command = [
        args.tcpdump,
        "-nnvvv",
        "-tt",
        "-r",
        str(args.output),
        f"host {args.host} and udp port 5430",
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "could not read the pcap")

    packets: list[Packet] = []
    current_header = None
    current_source = None
    for line in result.stdout.splitlines():
        header = IP_HEADER_RE.search(line)
        if header:
            current_header = header
            current_source = None

        source = SOURCE_RE.search(line)
        if source:
            current_source = source
        udp_length = UDP_LENGTH_RE.search(line)
        if not current_header or not current_source or not udp_length:
            continue
        packets.append(
            Packet(
                source=current_source.group("source"),
                ipv4_length=int(current_header.group("length")),
                udp_length=int(udp_length.group("length")),
                fragment_offset=int(current_header.group("offset")),
                flags=current_header.group("flags").strip(),
            )
        )
        current_header = None
        current_source = None
    return packets


def report(args: argparse.Namespace, packets: list[Packet]) -> int:
    if not packets:
        print("summary packets=0 result=fail")
        print("no matching IPv4 RTP packets were decoded from the pcap", file=sys.stderr)
        return 1

    fragmented = [packet for packet in packets if packet.fragmented]
    oversized = [packet for packet in packets if packet.udp_length > 1400]
    max_udp = max(packet.udp_length for packet in packets)
    max_ipv4 = max(packet.ipv4_length for packet in packets)
    source_addresses = sorted({packet.source for packet in packets})
    passed = not fragmented and not oversized and args.host in source_addresses

    print(f"capture_file={args.output}")
    print(f"source_addresses={','.join(source_addresses)}")
    print(f"packets={len(packets)}")
    print(f"max_udp_payload={max_udp}")
    print(f"max_ipv4_total={max_ipv4}")
    print(f"fragmented_packets={len(fragmented)}")
    print(f"oversized_udp_packets={len(oversized)}")
    print(f"result={'pass' if passed else 'fail'}")
    if fragmented:
        print(
            f"fragment_example=offset:{fragmented[0].fragment_offset} "
            f"flags:[{fragmented[0].flags}]",
            file=sys.stderr,
        )
    return 0 if passed else 1


def main() -> int:
    args = parse_args()
    if args.duration <= 0:
        print("duration must be positive", file=sys.stderr)
        return 2
    try:
        interface = args.interface or default_interface()
        capture(args, interface)
        return report(args, parse_tcpdump(args))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"tcpdump validation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
