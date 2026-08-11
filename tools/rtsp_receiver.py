#!/usr/bin/env python3
"""Receive and measure the ESP32-P4 RFC 2435 RTP/JPEG stream."""

import argparse
import os
import sys
import time
import zlib

# OpenCV reads this only when its FFmpeg backend is initialized.
os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;udp"

import cv2  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "url",
        nargs="?",
        default="rtsp://192.168.1.140:554/",
        help="RTSP stream URL",
    )
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--expected-fps", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=800)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--max-gap", type=float, default=0.6)
    parser.add_argument("--minimum-delivery", type=float, default=0.95)
    parser.add_argument(
        "--report-every",
        type=int,
        default=1,
        help="print every Nth decoded frame; the summary is always printed",
    )
    return parser.parse_args()


def open_capture(url: str) -> cv2.VideoCapture:
    parameters = []
    if hasattr(cv2, "CAP_PROP_OPEN_TIMEOUT_MSEC"):
        parameters += [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 10_000]
    if hasattr(cv2, "CAP_PROP_READ_TIMEOUT_MSEC"):
        parameters += [cv2.CAP_PROP_READ_TIMEOUT_MSEC, 3_000]
    return cv2.VideoCapture(url, cv2.CAP_FFMPEG, parameters)


def main() -> int:
    args = parse_args()
    if args.duration <= 0 or args.expected_fps <= 0 or args.report_every <= 0:
        print("duration, expected FPS, and report interval must be positive",
              file=sys.stderr)
        return 2

    capture = open_capture(args.url)
    if not capture.isOpened():
        print(f"failed to open {args.url} with the OpenCV FFmpeg backend",
              file=sys.stderr)
        return 2

    frames = 0
    dropped_estimate = 0
    changed_frames = 0
    longest_gap = 0.0
    measurement_start = None
    previous_received = None
    previous_checksum = None
    dimensions_valid = True
    warmup_discarded = 0
    warmup_previous = None
    warmup_gap = 0.5 / args.expected_fps

    try:
        while True:
            ok, frame = capture.read()
            received = time.monotonic()
            received_ns = time.time_ns()
            if not ok:
                print("frame read failed or timed out", file=sys.stderr)
                break

            if measurement_start is None:
                if warmup_previous is None:
                    warmup_previous = received
                    warmup_discarded += 1
                    continue
                if received - warmup_previous < warmup_gap:
                    warmup_previous = received
                    warmup_discarded += 1
                    continue
                measurement_start = received
                print(f"warmup_discarded={warmup_discarded}", flush=True)
            elif received - measurement_start >= args.duration:
                break

            frames += 1
            height, width = frame.shape[:2]
            dimensions_valid &= width == args.width and height == args.height

            gap = 0.0 if previous_received is None else received - previous_received
            if previous_received is not None:
                longest_gap = max(longest_gap, gap)
                dropped_estimate += max(
                    int(round(gap * args.expected_fps)) - 1, 0
                )
            previous_received = received

            checksum = zlib.crc32(frame[::32, ::32].tobytes())
            if previous_checksum is not None and checksum != previous_checksum:
                changed_frames += 1
            previous_checksum = checksum

            elapsed = max(received - measurement_start, 1e-9)
            rolling_fps = 0.0 if frames == 1 else (frames - 1) / elapsed
            if frames == 1 or frames % args.report_every == 0:
                print(
                    f"frame={frames} width={width} height={height} "
                    f"received_ns={received_ns} fps={rolling_fps:.2f} "
                    f"gap_ms={gap * 1000:.1f} "
                    f"dropped_estimate={dropped_estimate} "
                    f"longest_gap_ms={longest_gap * 1000:.1f}",
                    flush=True,
                )
    except KeyboardInterrupt:
        pass
    finally:
        capture.release()

    if measurement_start is None:
        print("summary frames=0 valid=no")
        return 3

    elapsed = max((previous_received or measurement_start) - measurement_start,
                  0.0)
    expected_frames = max(int(round(args.duration * args.expected_fps)), 1)
    delivery = frames / expected_frames
    average_fps = 0.0 if frames < 2 or elapsed == 0 else (frames - 1) / elapsed
    changing = frames <= 1 or changed_frames > 0
    passed = (
        dimensions_valid
        and changing
        and delivery >= args.minimum_delivery
        and longest_gap <= args.max_gap
        and elapsed >= args.duration - (1.5 / args.expected_fps)
    )
    print(
        f"summary frames={frames} expected={expected_frames} "
        f"delivery={delivery:.3f} average_fps={average_fps:.2f} "
        f"longest_gap_ms={longest_gap * 1000:.1f} "
        f"dropped_estimate={dropped_estimate} warmup_discarded={warmup_discarded} "
        f"changing={'yes' if changing else 'no'} "
        f"valid={'yes' if passed else 'no'}"
    )
    return 0 if passed else 4


if __name__ == "__main__":
    raise SystemExit(main())
