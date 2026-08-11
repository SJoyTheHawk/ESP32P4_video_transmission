#!/usr/bin/env python3
"""Display the ESP32-P4 RTSP/RTP JPEG stream with OpenCV."""

import argparse
from collections import deque
import os
import sys
import time

# OpenCV reads this setting when its FFmpeg backend is initialized.
os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;udp"

import cv2  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "url",
        nargs="?",
        default="rtsp://192.168.1.140:554/",
        help="RTSP stream URL",
    )
    parser.add_argument("--title", default="ESP32-P4 RTSP Stream")
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
    capture = open_capture(args.url)
    if not capture.isOpened():
        print(f"failed to open {args.url} with the OpenCV FFmpeg backend",
              file=sys.stderr)
        return 2

    window_created = False
    frame_times: deque[float] = deque(maxlen=30)
    frame_count = 0

    try:
        cv2.namedWindow(args.title, cv2.WINDOW_NORMAL)
        window_created = True
        print(f"viewing {args.url}; press q or Esc to close")

        while True:
            ok, frame = capture.read()
            if not ok:
                print("frame read failed or timed out", file=sys.stderr)
                return 3

            frame_count += 1
            frame_times.append(time.monotonic())
            fps = 0.0
            if len(frame_times) > 1:
                fps = (len(frame_times) - 1) / (frame_times[-1] - frame_times[0])

            height, width = frame.shape[:2]
            cv2.putText(
                frame,
                f"{width}x{height}  {fps:.1f} FPS",
                (16, 32),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2,
                cv2.LINE_AA,
            )
            cv2.imshow(args.title, frame)

            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            if cv2.getWindowProperty(args.title, cv2.WND_PROP_VISIBLE) < 1:
                break
    except cv2.error as error:
        print(f"OpenCV viewer error: {error}", file=sys.stderr)
        return 4
    except KeyboardInterrupt:
        pass
    finally:
        capture.release()
        if window_created:
            cv2.destroyAllWindows()

    print(f"viewer closed after {frame_count} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
