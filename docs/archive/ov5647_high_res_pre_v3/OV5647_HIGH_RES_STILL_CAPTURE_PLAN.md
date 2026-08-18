# OV5647 High-Resolution Still Capture and Photo Refresh Plan

## Goal

Keep a low-bandwidth VGA RTSP/RTP-JPEG stream available while adding a
retained high-resolution JPEG photo resource. Clients discover new photos by
polling metadata and avoid downloading the same photo twice with HTTP
conditional requests.

The firmware supports two capture modes:

- `background`: capture and publish a new high-resolution photo on a schedule,
  defaulting to one photo per second for data-collection workloads.
- `on_demand`: capture only after an explicit capture request from the settings
  page or control API.

The OV5647 has one image pipeline. A high-resolution capture therefore stops
VGA frame production while the sensor mode, capture buffers, and JPEG encoder
are changed. The RTSP control session must remain alive, but media gaps are
expected during the transaction.

## Existing Code and Baseline

The current firmware is a single Arduino sketch in
`mipi_csi_camera/mipi_csi_camera.ino`. It owns camera initialization, V4L2
capture, hardware JPEG encoding, Wi-Fi, and the RTSP server. There is no
existing HTTP server, retained photo store, metadata model, or persistent
capture-mode configuration.

The README and the current working tree do not currently agree on all stream
quality and frame-interval values. Phase 0 must record the actual baseline and
make the selected VGA quality and frame rate explicit before feature work.

## Sensor and Driver Findings

The OV5647 active array is 2592x1944. It has one output pipeline and cannot
produce VGA and 5 MP frames independently at the same time.

The installed Arduino ESP32 core currently exposes these OV5647 modes:

- 800x1280 RAW8 at 50 FPS
- 800x640 RAW8 at 50 FPS
- 800x800 RAW8 at 50 FPS

It does not expose a 640x480 mode. Inspection of the installed CSI driver
confirms that `VIDIOC_S_FMT` rejects dimensions different from the active
sensor mode. Phase 0 includes a runtime readback probe to verify that behavior
on the board. Phase 1 will add a repository-local 640x480 OV5647
descriptor/register table. A measured 800x640-to-640x480 resize remains a
fallback only if the direct sensor mode cannot be validated.

The latest upstream Espressif driver provides 1920x1080 RAW10 but not
2592x1944. The application will therefore supply repository-local
high-resolution mode descriptions rather than replacing the complete Arduino
core.

## Selected Behavior

### VGA RTSP stream

| Setting | Target |
| --- | --- |
| Resolution | 640x480 VGA |
| Capture format | RGB565 |
| JPEG quality | 50 |
| Frame rate | 10 FPS |
| Capture buffers | 2 |
| RTSP port | TCP 554 |
| RTP/JPEG port | UDP 5430 |

The packetizer must receive only VGA JPEGs. High-resolution still JPEGs must
never be passed to the RTSP path.

### High-resolution photos

| API value | Resolution | Sensor output | Capture format | JPEG quality | Max JPEG |
| --- | ---: | --- | --- | ---: | ---: |
| `1080p` | 1920x1080 | RAW10, up to 30 FPS | YUV420 | 90 | 2 MiB |
| `5mp` | 2592x1944 | RAW10, up to 15 FPS | YUV420 | 90 | 4 MiB |

YUV420 reduces the 5 MP input buffer to approximately 7.2 MiB compared with
approximately 9.6 MiB for RGB565. Only one high-resolution capture buffer is
allocated at a time.

## HTTP API

Start an ESP-IDF HTTP server on TCP port 80 after Wi-Fi and camera
initialization. The API is intended for a trusted LAN and does not add
authentication in this phase.

### Download the retained photo

```text
GET /api/photo/latest.jpg
GET /api/photo/latest.jpg?id=<photo_id>
```

The endpoint returns the latest retained JPEG. A requested `id` prevents a
metadata/download race: the handler returns that exact retained version or
reports that it is no longer available. Omitting `id` returns the current
latest version.

Successful responses include:

```text
HTTP/1.1 200 OK
Content-Type: image/jpeg
Cache-Control: no-store
ETag: "<boot-generation>-<photo-id>"
X-Photo-Id: <photo-id>
X-Image-Width: <width>
X-Image-Height: <height>
X-Capture-Time-Ms: <measured duration>

<JPEG bytes>
```

Clients should send `If-None-Match` with the last ETag. If the photo is
unchanged, return `304 Not Modified` with no body. Return `404` before the
first successful capture and `410 Gone` when a specifically requested old
photo has already been released.

### Poll photo metadata

```text
GET /api/photo/metadata
```

The endpoint returns a small JSON document. When the state is `ready`, it
supports the same photo `If-None-Match`/`304` behavior. During `capturing` or
an error state, return `200` with the current state even when the published
photo ETag has not changed.

```json
{
  "state": "ready",
  "photo_id": 42,
  "boot_generation": "a1b2c3d4",
  "download_url": "/api/photo/latest.jpg?id=42",
  "width": 2592,
  "height": 1944,
  "bytes": 1834210,
  "resolution": "5mp",
  "jpeg_quality": 90,
  "capture_duration_ms": 842,
  "captured_at_ms": 123456789,
  "capture_mode": "background"
}
```

Before the first photo, return `200` with `"state": "empty"`. During a
refresh, retain the previous ready photo and report `"state": "capturing"`.
After a failed refresh, retain the previous photo and report an error state.

`photo_id` is monotonic for the current boot. `boot_generation`, derived from
`esp_random()`, prevents a reboot from being mistaken for a new photo with a
reused counter. Wall-clock time is diagnostic metadata only and is not the
freshness key.

### Settings and capture control

The settings page is served from `/` and uses these supporting routes:

```text
GET /api/settings
PUT /api/settings
POST /api/photo/capture
```

`PUT /api/settings` validates and persists capture mode, interval, resolution,
and any RTSP policy. `POST /api/photo/capture` queues an explicit capture and
returns `202 Accepted`; overlapping requests return `409`. These routes are
supporting controls and are separate from the two core photo resources.

## Camera Pipeline

Add a repository-local capture controller around the V4L2 device. It owns the
capture file descriptor, MMAP buffers, selected output format, sensor
descriptors, and capture timeout. `ESPVideoClass` remains responsible for
initializing the ESP32-P4 MIPI-CSI hardware.

The controller must provide:

- Read and retain the driver-selected VGA sensor mode during startup.
- Stop capture and release all MMAP buffers.
- Apply an `esp_cam_sensor_format_t` with `VIDIOC_S_SENSOR_FMT` while capture
  is stopped.
- Reopen capture with one or two buffers and select RGB565 or YUV420.
- Capture with a bounded dequeue timeout.
- Clean up safely after every partial initialization failure.

The 1920x1080 mode table is based on Espressif's Apache-licensed upstream
OV5647 implementation. The 2592x1944 table is derived from public OV5647
register documentation and validated against full-resolution timing data. The
initial 5 MP target is:

- Pixel clock: approximately 87.5 MHz
- Horizontal total size: 2844 pixels
- Vertical total size: 1968 lines
- MIPI line rate: approximately 437.5 Mbps per lane
- Output: 2592x1944 RAW10 at up to 15 FPS

Each mode table must include reset, PLL, crop, output size, timing, exposure,
gain, and MIPI configuration. It must not depend on register state left by a
preceding mode.

### Capture timeout feasibility gate

The installed `ESPVideoCaptureDevClass::captureBuffer()` uses a blocking
`VIDIOC_DQBUF`, and the ESP-Video VFS registration has no `select()`/`poll()`
hooks. A sketch-only timeout cannot reliably interrupt that call. The capture
controller must therefore vendor or patch ESP-Video to use a finite FreeRTOS
wait when dequeuing before timeout error handling is accepted.

## JPEG Encoder and Photo Store

Extend `JpegEncoderClass` so it can be stopped and recreated for a new
resolution, input format, quality, output capacity, and timeout.

The still encoder must accept YUV420, generate quality-90 JPEG, enforce the
2 MiB/4 MiB limits, and validate SOI/EOI markers. Completed still buffers must
be detachable from the encoder before the streaming encoder is recreated.

Add a reference-counted immutable `PhotoStore`:

- Capture allocates and prepares a new JPEG before publishing it.
- Publication atomically swaps the latest photo and increments `photo_id`.
- HTTP handlers acquire a reference before sending the body.
- The previous photo is freed only after all active downloads release it.
- Allocation or encoding failure preserves the previous photo and ID.
- Metadata and download responses refer to the same photo version.

Log total PSRAM, free PSRAM, largest free block, retained-photo size, and
encoder/capture timings before and after every transaction. Reserve at least
1 MiB after accounting for the YUV420 buffer, JPEG capacity, driver overhead,
and the retained-photo store.

Do not silently reduce resolution or quality when memory is insufficient.

## Capture Transaction

The HTTP handlers, background scheduler, and RTSP loop share the camera and
hardware JPEG engine. Serialize them with a camera mutex and an atomic busy
flag.

For every capture:

1. Claim the operation; return `409` for an overlapping explicit request.
2. Lock the pipeline and make the RTSP loop skip frame capture.
3. Stop VGA capture and release its two buffers and streaming encoder.
4. Apply the requested high-resolution sensor mode.
5. Start one-buffer YUV420 capture and the quality-90 encoder.
6. Discard at least three valid settling frames.
7. Capture and encode the next frame; detach the JPEG buffer.
8. Stop high-resolution capture and release high-resolution resources.
9. Restore the saved 640x480 mode, two RGB565 buffers, and quality-50 encoder.
10. Discard restored VGA frames.
11. Publish the detached JPEG to `PhotoStore`.
12. Unlock the pipeline and resume RTSP media.

Every failure after step 3 uses the same restoration path. For an explicit
capture request, a restoration failure returns `500`. For a scheduled
capture, it records the failure in metadata. In both cases it marks the camera
unavailable, keeps the HTTP service alive, and logs the exact failed
operation.

In `background` mode, the scheduler runs this transaction at the configured
interval. The interval is measured from completion of the previous attempt;
missed intervals are not queued and captures never overlap. In `on_demand`
mode, the transaction runs only after an explicit request.

Because one-second background capture repeatedly interrupts the single sensor
pipeline, continuous VGA media is not an acceptance criterion for background
mode. On-demand mode must preserve the RTSP session and resume after a
bounded pause. Background mode may tolerate measured media gaps or disable
RTSP media through a settings policy.

## Implementation Phases

Each phase has one primary risk and must pass its exit condition before the
next phase starts.

### Phase 0: Baseline, VGA feasibility, and diagnostics

- Reconcile the current working-tree stream quality and interval with the
  documented target.
- Record actual stream dimensions, FPS, JPEG size, PSRAM use, and timings.
- Confirm sensor PID `0x5647`.
- Determine whether direct 640x480 output is available or whether a local
  sensor mode or measured resize fallback is required.
- Validate timed V4L2 dequeue support.

**Exit condition:** the baseline VGA path and timeout approach are known and
documented.

**Implementation status (2026-08-18):** the firmware now uses the selected
quality-50, 10 FPS target; logs heap/PSRAM and active formats; confirms the
OV5647 through ESP-Video's register-based auto-detection; probes VGA format
readback; reports the blocking-dequeue limitation; and emits five-second
rolling FPS, JPEG-size, capture, encode, send, and drop measurements. Normal
and camera-only ESP32-P4 builds pass with Arduino core 3.3.11. Phase 0 remains
open until the firmware is flashed to the physical board and the serial/RTSP
measurements and VGA probe result are recorded.

### Phase 1: VGA and still sensor mode definitions

- Add and validate the direct 640x480 mode or selected fallback.
- Add the repository-local 1920x1080 RAW10 descriptor and register table.
- Add a camera-only diagnostic that applies a still mode and restores VGA.

**Exit condition:** the sensor accepts the selected modes and repeatedly
returns to VGA without rebooting or leaving CSI in an error state.

### Phase 2: Capture lifecycle and mode switching

- Add the V4L2 capture controller with stream stop/start, MMAP ownership,
  buffer-count changes, and bounded capture.
- Implement `VGA -> 1080p -> capture -> VGA`.
- Keep high-resolution JPEG delivery out of RTSP.

**Exit condition:** repeated switches produce valid 1920x1080 buffers and
restore 640x480 streaming.

### Phase 3: JPEG lifecycle and retained photo store

- Add encoder `end()` and reinitialization support.
- Make input format, quality, output capacity, and timeout configurable.
- Add YUV420 support, output ownership transfer, `PhotoStore`, IDs, ETags,
  and metadata snapshots.

**Exit condition:** known YUV420 data produces valid JPEG and repeated encoder
and store replacement does not reduce free PSRAM.

### Phase 4: Standalone HTTP service

- Start `esp_http_server` on port 80.
- Implement conditional metadata and JPEG downloads.
- Implement settings and explicit-capture support routes.

**Exit condition:** clients can poll metadata, receive `304` when unchanged,
download the matching JPEG, and handle empty/capturing/error states.

### Phase 5: RTSP and photo integration

- Add camera mutex and busy state.
- Make RTSP skip capture during the transaction.
- Restore VGA before publishing the photo.
- Verify connected RTSP sessions resume after successful and failed captures.

**Exit condition:** on-demand capture works with and without an active RTSP
client and does not require RTSP reconnect.

### Phase 6: NVS settings and background capture

- Persist `capture_mode`, `interval_ms`, and resolution through `Preferences`.
- Add the settings page and runtime mode changes.
- Add the background scheduler with a one-second default interval.
- Measure capture cadence, memory stability, and RTSP media gaps.

**Exit condition:** settings changes apply at transaction boundaries and 100
background captures complete without resource growth.

### Phase 7: Full 5 MP sensor mode

- Add and validate the 2592x1944 RAW10 register profile.
- Enforce the 4 MiB JPEG limit and PSRAM reserve.
- Make `5mp` the default only after physical validation.

**Exit condition:** the API returns decodable 2592x1944 quality-90 JPEGs and
the measured memory margin is recorded.

### Phase 8: Final hardening and documentation

- Exercise every documented error, timeout, disconnect, and restoration path.
- Run 100 captures per mode.
- Document measured VGA behavior, photo latency, memory margins, and RTSP gaps
  in the README without exposing Wi-Fi credentials.

## Test Plan

### Build and static checks

- Compile normal Wi-Fi/RTSP and `EXCLUDE_WIFI` builds.
- Confirm the packetizer accepts 640x480 and rejects accidental high-resolution
  input from the stream path.
- Add host tests for metadata JSON, ETag generation, ID matching, and
  reference-counted photo replacement.

### Functional tests

- Verify decoded RTSP frames are 640x480 at the selected baseline FPS.
- Apply every sensor descriptor and verify readback.
- Switch repeatedly between VGA and 1080p, then restore VGA.
- Encode known YUV420 data and validate JPEG markers and dimensions.
- Poll metadata and verify `photo_id` changes only after successful publish.
- Verify unchanged ETags return `304` with no body.
- Verify the metadata URL and JPEG body identify the same photo.
- Verify stale requested IDs return the documented unavailable status.
- Verify empty, capturing, failed-refresh, and camera-unavailable states.
- Verify orientation, color order, and field of view with a color chart.

### Streaming and reliability tests

- Keep an RTSP receiver connected while repeating on-demand captures.
- Run background capture at one-second intervals and measure actual cadence.
- Record media gaps separately for on-demand and background modes.
- Run at least 100 captures without rebooting for each still resolution.
- Confirm PSRAM, MMAP buffers, encoder handles, and retained blobs do not trend
  downward or grow.
- Disconnect HTTP clients during JPEG transfer and verify reference cleanup.

### Failure tests

- Unsupported resolution: `400`.
- Overlapping capture: `409`.
- No photo yet: `404`.
- Requested old photo released: `410`.
- Allocation failure: `503`, with VGA restoration.
- Capture or encoding timeout: `504`, with VGA restoration.
- Sensor-mode failure: restore saved VGA mode.
- Restoration failure: `500`, camera marked unavailable, HTTP service alive.

## Acceptance Criteria

- The RTSP stream produces compatible 640x480 JPEG frames at the documented
  baseline.
- The API returns decodable 1920x1080 and 2592x1944 quality-90 photos.
- Metadata polling reliably identifies new photos without retransmitting old
  JPEGs.
- Conditional downloads return `304` for an unchanged photo.
- The previous photo remains available while a refresh is running or fails.
- On-demand capture preserves the RTSP session and restores VGA streaming.
- Background capture runs at the configured cadence without resource leaks.
- All documented memory, timeout, busy, and restoration errors are observable.

## Out of Scope

- Simultaneous independent sensor output at VGA and high resolution.
- Continuous 5 MP streaming.
- Multiple concurrent high-resolution captures.
- Authentication for an untrusted network.
- Runtime JPEG-quality selection.
- Changing RTSP transport, ports, packetizer protocol, or client count.

## References

- [OV5647 camera module specification](https://files.seeedstudio.com/wiki/OV5647_Series_Camera_Module/OV5647-62.pdf)
- [OV5647 datasheet](https://www.welectron.com/mediafiles/productimg/arducam/Image_Sensor/OV5647_DS.pdf)
- [Espressif upstream OV5647 driver](https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/sensors/ov5647/ov5647.c)
- [Linux OV5647 driver](https://kernel.googlesource.com/pub/scm/linux/kernel/git/klassert/ipsec-next/+/refs/heads/master/drivers/media/i2c/ov5647.c)
- [ESP32-P4 JPEG encoder documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/jpeg.html)
