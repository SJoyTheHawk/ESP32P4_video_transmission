# Phase 4 Arduino Test

The `v3.0-phase4-arduino` sketch runs the 1080p still transaction during boot.
It keeps the candidate JPEG internal, then restores the 800x800 RGB565,
two-buffer, quality-50 baseline before starting RTSP. Arduino-ESP32 3.3.11's
precompiled ESP-Video CSI driver rejects a one-buffer request, so the 1080p
transaction uses its required two MMAP buffers.

Compile with the 32 MiB partition profile and upload from Arduino IDE. The
serial monitor must show these gates:

```text
implementation version=v3.0-phase4-arduino
high-res memory gate status=passed ...
high-res sensor mode=MIPI_2lane_24Minput_RAW10_1920x1080_30fps output=1920x1080 format=RGB565 fps=30 buffers=2
high-res capture validation status=passed completed=20 requested=20 state=BaselineRunning
RTSP/RTP-JPEG stream ready: rtsp://<board-ip>:554/
```

The board must not report the Phase 4 gate failure. After boot, verify that
RTSP remains the baseline dimensions and cadence:

```sh
python3 tools/rtsp_receiver.py \
  rtsp://<board-ip>:554/ \
  --duration 300 \
  --expected-fps 10 \
  --width 800 \
  --height 800 \
  --max-gap 0.6 \
  --minimum-delivery 0.95 \
  --report-every 30
```

The Phase 4 gate is complete only when all 20 transactions pass, baseline
restoration succeeds, and the five-minute RTSP receiver result is valid. No
1080p JPEG should appear in the RTP stream.
