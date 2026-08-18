# Phase 3 Arduino Test

## Scope

Firmware `v3.0-phase3-arduino` moves the active 800x800 RGB565 path behind
`CaptureController`. The controller is the only active owner of the V4L2 video
descriptor, MMAP buffers, dequeue timeout, stream state, and baseline JPEG
encoder. RTSP consumes controller-provided frames only.

The Arduino Wi-Fi path disables modem power save before `WiFi.STA.begin()`.
This is an isolated ESP-Hosted C6 mitigation for the confirmed long-run UDP
RTP packet-loss events; it does not change capture or JPEG behavior.

At boot, the controller validates the Phase 2 bounded-dequeue recovery path,
then performs a warm-up restart and three measured baseline restart cycles.
Each restart stops the stream, releases MMAP buffers, closes the device,
reopens it, reapplies RGB565 and the dequeue timeout, remaps buffers, starts
capture, and verifies a frame. The warm-up excludes the precompiled driver's
one-time PSRAM free-list reshaping from the repeated-cycle trend measurement.
After each restart the controller permits up to three bounded dequeue attempts,
with a 50 ms settle interval, before declaring first-frame recovery failed.

## Expected Serial Results

The boot log must contain:

```text
implementation version=v3.0-phase3-arduino
Wi-Fi modem power save: disabled
capture controller baseline width=800 height=800 format=RGB565 buffers=2 state=BaselineRunning
capture timeout test status=passed injection=hold-available-buffers requested_ms=5000 elapsed_ms=... recovery_frame=valid
capture restart validation status=passed warmup=passed cycles=3 state=BaselineRunning heap_before=... heap_after=... psram_before=... psram_after=...
RTSP/RTP-JPEG stream ready: rtsp://192.168.1.91:554/
```

The timeout elapsed value must be 4500-6500 ms. `driver_errno=1` and
`reported_errno=116` remain expected for Arduino-ESP32 3.3.11.
Heap and PSRAM free/largest values after the restart cycles must not be lower
than their corresponding before values.

## RTSP Parity Test

After the RTSP-ready line, run:

```bash
python3 tools/rtsp_receiver.py rtsp://192.168.1.91:554/ \
  --duration 300 \
  --expected-fps 10 \
  --width 800 \
  --height 800 \
  --max-gap 0.6 \
  --minimum-delivery 0.95 \
  --report-every 30
```

## Exit Gate

Phase 3 passes when all controller boot gates pass and the RTSP receiver ends
with `valid=yes`. Any `Phase 3 gate failed` output blocks Phase 4.
