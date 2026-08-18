# Phase 7 Arduino Test

## Scope

Phase 7 measures how the existing RTSP/RTP-JPEG stream behaves while an
on-demand 1080p photo capture temporarily takes ownership of the camera. It
also records the status of the V3 640x480 release-stream target.

This is not a keepalive implementation phase. Do not change RTSP keepalive,
RTCP, transport, or reconnect behavior unless a measured client failure
identifies a specific need.

## Preconditions

- Phase 6 has passed: the HTTP photo API is ready and one 1080p capture has
  completed successfully.
- Use the current published RTSP URL:

  ```text
  rtsp://192.168.1.91:554/
  ```

- The server supports a single unicast UDP RTP/JPEG client. RTP-over-TCP,
  multicast, authentication, and multiple simultaneous clients are not test
  targets.
- Record the baseline dimensions printed by the serial monitor. The current
  Arduino-only baseline is 800x800 RGB565 at JPEG quality 50 and 10 FPS unless
  a separately proven 640x480 hardware path is enabled.

## Procedure

Run one RTSP client at a time. Let it receive media for at least 30 seconds,
then trigger exactly one photo capture from another terminal:

```sh
curl -i -X POST -H 'Content-Type: application/json' \
  -d '{}' http://192.168.1.91/api/photo/capture
```

Keep the client connected until the stream has resumed for at least 30 seconds.
Then close it, reconnect it to the same URL, and verify that it receives media
without an ESP32 reboot.

Record these results for each client:

| Field | Record |
| --- | --- |
| Client and version | Installed client/version |
| Baseline dimensions | Decoded dimensions before capture |
| Capture request | HTTP status and body |
| Media gap | Longest observed gap in ms |
| Existing session | Media resumed / timed out / disconnected |
| Reconnect | Pass / fail without ESP32 reboot |
| Baseline after capture | Decoded dimensions after restoration |
| Client diagnostics | Relevant warning or error text |

### OpenCV Measurement

This is the quantitative acceptance measurement for the current 800x800
baseline. Start it first, issue the capture after 30 seconds, and retain the
summary output.

```sh
python3 tools/rtsp_receiver.py \
  rtsp://192.168.1.91:554/ \
  --duration 300 --expected-fps 10 --width 800 --height 800 \
  --max-gap 0.6 --minimum-delivery 0.95 --report-every 30
```

Pass criteria for this run are `valid=yes`, dimensions remaining 800x800, and
the recorded longest gap no greater than 600 ms. `dropped_estimate` is an
arrival-gap estimate, not proof of RTP sequence loss.

### FFmpeg

The server accepts UDP transport only. Start the command, trigger one capture,
then stop it after media has resumed.

```sh
ffmpeg -rtsp_transport udp \
  -i rtsp://192.168.1.91:554/ \
  -f null -
```

Record whether FFmpeg keeps decoding in the existing RTSP session, and whether
it can reconnect after the process is stopped.

### GStreamer

Use UDP transport and the static RTP/JPEG payload advertised by the server:

```sh
gst-launch-1.0 -v \
  rtspsrc location=rtsp://192.168.1.91:554/ protocols=udp latency=100 ! \
  application/x-rtp,media=video,encoding-name=JPEG,payload=26 ! \
  rtpjpegdepay ! jpegdec ! \
  fpsdisplaysink video-sink=fakesink text-overlay=false sync=false
```

Record the same interruption and reconnect behavior. If the local GStreamer
build requires different caps syntax, record the working pipeline alongside the
result rather than changing firmware first.

### VLC

Open `rtsp://192.168.1.91:554/` as a network stream and use UDP transport.
Trigger one capture while it is playing. Record whether it resumes in the same
session, its visible interruption, and whether a close/reopen reconnect works.

## RTP Packet Check

Run the raw RTP probe before or after the connected-capture measurements. It
verifies packet order, offsets, quantization tables, marker placement, and the
actual 800x800 RTP dimensions.

```sh
python3 tools/rtp_packet_probe.py \
  rtsp://192.168.1.91:554/ \
  --frames 3000 --timeout 5
```

The expected result contains `sequence=monotonic`, `offsets=valid`,
`marker=final-only`, and `result=pass`. A sequence gap is a failure of this
strict probe, even when the decoded receiver later reaches its delivery target.

## 640x480 Target Record

The V3 release target is 640x480. Phase 7 must establish one of these two
outcomes:

1. **Validated:** a complete direct 640x480 sensor descriptor exists, or an
   800x640 mode has a proven hardware/ISP crop or resize. V4L2 readback and
   decoded RTP JPEG dimensions must both be 640x480.
2. **Blocked:** no direct descriptor is available and the Arduino ESP-Video
   binary does not expose a usable hardware crop/resize path. Record the exact
   error and retain the measured 800x800 baseline. Do not call 800x800 or
   800x640 output VGA.

The current Arduino-only crop probe returned:

```text
video->ops->set_selection=106
release stream status=blocked stage=hardware-crop errno=3
```

This means ESP-Video returned `ESP_ERR_NOT_SUPPORTED` (`106`). It is a release
blocker for 640x480, not a reason to add software cropping or an RTSP keepalive
change during this phase.

## Phase Exit Record

Create a short result record containing the three client rows, OpenCV summary,
RTP-probe summary, serial baseline dimensions, and either the validated
640x480 evidence or the blocker above.

Phase 7 is complete when on-demand capture restores RTSP for every tested
client without an ESP32 reboot, and the 640x480 target is either measured as
real output or explicitly documented as blocked.
