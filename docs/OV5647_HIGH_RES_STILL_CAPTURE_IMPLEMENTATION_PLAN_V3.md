# OV5647 High-Resolution Still Capture Implementation Plan V3

**Version:** 3.0  
**Date:** 2026-08-18  
**Status:** Authoritative plan; Phase 0 code complete, hardware validation pending  
**Supersedes:** All earlier OV5647 high-resolution still-capture plans, reviews,
responses, correction notes, and execution guides

## 1. Goal

Add a retained high-resolution JPEG resource to the ESP32-P4 firmware while
preserving the existing RTSP/RTP-JPEG service. Clients discover a newly
published photo by polling metadata and download that exact version through a
separate HTTP endpoint.

The camera has one sensor pipeline. A high-resolution capture therefore pauses
RTSP media, changes the sensor and capture pipeline, captures one photo, restores
the streaming mode, and only then publishes the photo. The RTSP control session
remains active when the client and measured transaction duration permit it.

## 2. Final Decisions

These decisions are binding for implementation:

1. The production build is an ESP-IDF 5.5.x application with Arduino core
   3.3.11 used as a component. This preserves the existing Arduino Wi-Fi and
   RTSP code while allowing source-level control of ESP-Video.
2. Pin `espressif/esp_video` 0.5.1 and its compatible camera dependencies in the
   project. Do not patch the precompiled Arduino archive or copy only the
   `ESP_Video` wrapper.
3. Patch `esp_video/src/esp_video_ioctl.c` so `VIDIOC_DQBUF` uses a finite,
   configurable FreeRTOS wait and returns `ESP_ERR_TIMEOUT`, which the VFS maps
   to `ETIMEDOUT`.
4. High-resolution capture is disabled unless bounded dequeue and restoration
   pass fault-injection tests. There is no watchdog-only supported tier.
5. A single `CaptureController` owns the V4L2 fd, sensor formats, MMAP buffers,
   capture state, and encoder lifecycle. No second camera owner is allowed.
6. The required still-photo path is 1920x1080 RGB565 input at JPEG quality 90.
   YUV420 and 2592x1944 are optional later capabilities.
7. The release stream target is 640x480 RGB565, quality 50, 10 FPS, two capture
   buffers. The current 800x800 mode remains the bring-up fallback until a VGA
   sensor/output path is proven on hardware.
8. Use ESP-IDF `esp_http_server`; do not add AsyncWebServer.
9. Capture requests are asynchronous. `POST /api/photo/capture` queues work and
   returns immediately. JPEG download never owns the camera transaction.
10. Publish a new photo only after the streaming baseline is restored. A failed
    restoration discards the candidate photo and marks the camera unavailable.
11. `PhotoStore` uses a mutex for pointer lookup/publication and a reference
    count for active HTTP downloads. Pointer lookup and reference acquisition
    happen under the same mutex.
12. For the pinned ESP-IDF JPEG API, output from `jpeg_alloc_encoder_mem()` is
    released with `free()`. Encapsulate this pairing in `JpegOutputBuffer` and
    verify it with repeated allocation tests before encoder reconfiguration.
13. Existing retained JPEGs remain downloadable when the camera is unavailable.
    Metadata reports the unavailable state and new capture requests return 503.
14. RTSP keepalive changes are measurement-driven. Do not add RTCP or OPTIONS
    behavior until a tested client requires it.
15. A background interval is the minimum delay after the previous attempt
    completes. Captures never overlap and missed intervals are not queued.

## 3. Current Baseline

The current Arduino sketch is `mipi_csi_camera/mipi_csi_camera.ino` and includes:

- OV5647 initialization through `ESPVideoClass`;
- 800x800 RGB565 capture;
- hardware JPEG encoding at quality 50;
- approximately 10 FPS scheduling;
- two capture buffers;
- RTSP control on TCP 554;
- RTP/JPEG on UDP 5430;
- Phase 0 sensor, format, memory, timeout, and performance diagnostics.

There is no HTTP server, retained photo store, high-resolution sensor mode,
bounded dequeue, background still scheduler, or persistent still settings.

The installed Arduino package uses ESP-IDF 5.5.5 and links ESP-Video as a
precompiled archive. The repository's `reference/17_mipicamera` project contains
source-managed ESP-Video 0.5.1 at commit
`7c343dc478f73e3234ed898eb358accd8de92ff7`, which is the initial source pin for
the production component build.

## 4. Supported Product Behavior

### 4.1 Stream modes

| Mode | Resolution | Format | JPEG quality | Target rate | Buffers |
| --- | ---: | --- | ---: | ---: | ---: |
| Bring-up fallback | 800x800 | RGB565 | 50 | 10 FPS | 2 |
| Release target | 640x480 | RGB565 | 50 | 10 FPS | 2 |

The release is not complete until the RTSP packetizer receives 640x480 JPEGs or
a documented hardware limitation is explicitly accepted. High-resolution JPEGs
must never enter the RTSP packetizer.

### 4.2 Still modes

| API value | Resolution | Capture input | JPEG quality | Status |
| --- | ---: | --- | ---: | --- |
| `1080p` | 1920x1080 | RGB565 | 90 | Required |
| `5mp` | 2592x1944 | RGB565 or proven YUV420 | 90 | Optional |

YUV420 is an optimization only after its V4L2 capture path and JPEG input path
both pass independent tests. It is not a fallback required by the core feature.

### 4.3 Capture modes

- `on_demand`: only explicit capture requests start a transaction.
- `background`: a timer queues a transaction after the configured minimum delay,
  default 1000 ms, measured from completion of the previous attempt.

Only one capture request may be queued or executing. An explicit request while
busy returns 409. Background ticks while busy are skipped.

## 5. HTTP Contract

### 5.1 Metadata

```text
GET /api/photo/metadata
```

Example ready response:

```json
{
  "state": "ready",
  "photo_id": 42,
  "boot_generation": "9a5416e7c8d2310f",
  "etag": "\"9a5416e7c8d2310f-42\"",
  "download_url": "/api/photo/latest.jpg?id=42",
  "width": 1920,
  "height": 1080,
  "bytes": 245760,
  "resolution": "1080p",
  "jpeg_quality": 90,
  "capture_duration_ms": 842,
  "captured_at_ms": 123456789,
  "capture_mode": "on_demand",
  "last_error": null
}
```

States are `empty`, `capturing`, `ready`, `error`, and `camera_unavailable`.
During capture or after failure, metadata retains the previous published photo
fields when one exists.

When state is `ready`, `If-None-Match` matching the current photo ETag returns
304 with no body. For `capturing`, `error`, or `camera_unavailable`, metadata
returns 200 so clients observe the state transition even if the photo ETag did
not change.

### 5.2 Download

```text
GET /api/photo/latest.jpg
GET /api/photo/latest.jpg?id=<photo_id>
```

Successful responses include:

```text
Content-Type: image/jpeg
Content-Length: <bytes>
Cache-Control: no-store
ETag: "<boot-generation>-<photo-id>"
X-Photo-Id: <photo-id>
X-Image-Width: <width>
X-Image-Height: <height>
X-Capture-Time-Ms: <duration>
```

Status behavior:

| Condition | Status |
| --- | ---: |
| Current photo exists | 200 |
| Matching `If-None-Match` | 304 |
| No photo published yet | 404 |
| Requested ID is not current/retained | 410 |
| Camera unavailable but an old photo exists | 200 for that photo |

The HTTP handler acquires a `PhotoBlob` reference, releases the store mutex,
sends the body, and releases its reference on success, disconnect, or error.

### 5.3 Capture control

```text
POST /api/photo/capture
Content-Type: application/json

{"resolution":"1080p"}
```

| Condition | Status |
| --- | ---: |
| Request queued | 202 |
| Request already queued or running | 409 |
| Invalid request | 400 |
| Camera unavailable or memory gate fails | 503 |

`photo_id` increments only after successful capture, encoding, baseline
restoration, and publication. It is not allocated when a request is queued.

### 5.4 Settings

Supporting endpoints are added after the core photo API:

```text
GET /api/settings
PUT /api/settings
```

Settings include capture mode, minimum interval, resolution, and RTSP policy.
Persist them with `Preferences` only after transaction behavior is stable.

## 6. Build Architecture

Create a production ESP-IDF application under `firmware/` with:

- ESP-IDF 5.5.x pinned for reproducible builds;
- Arduino core 3.3.11 as a component with `CONFIG_AUTOSTART_ARDUINO=y`;
- `CONFIG_FREERTOS_HZ=1000` as required by Arduino;
- source-managed `espressif__esp_video` 0.5.1;
- source-managed matching `esp_cam_sensor` and `esp_sccb_intf` components;
- the current sketch and RTSP sources ported with minimal behavioral changes;
- the board's existing 16 MiB flash/PSRAM configuration preserved.

The current `.ino` build remains available as the Phase 0 reference until the
ESP-IDF/Arduino-component build reproduces its RTSP output. After parity, the
ESP-IDF build becomes authoritative.

## 7. Component Architecture

```text
CaptureService
  receives explicit/background requests
  owns the one-entry request queue and busy state
              |
              v
CaptureController
  sole V4L2 owner and camera state machine
  owns streaming/still encoders and mode restoration
              |
              v
PhotoStore <------ PhotoApi (esp_http_server)
  immutable blob      acquires references for metadata/download
  version metadata

RTSP loop ------> CaptureController::acquireBaselineFrame()
```

### 7.1 CaptureController states

```text
Uninitialized
BaselineRunning
Stopping
SensorConfiguring
BufferAllocating
HighResReady
Capturing
Restoring
Unavailable
```

Only the controller performs V4L2 ioctls. Its transaction contract is:

1. Verify `BaselineRunning` and claim the request.
2. Run the runtime memory gate.
3. Stop baseline capture and destroy baseline buffers/encoder resources.
4. Apply the complete 1080p sensor descriptor.
5. Select RGB565 and allocate one capture buffer plus still encoder output.
6. Start capture and discard the measured number of settling frames.
7. Dequeue with the patched finite timeout and encode one JPEG.
8. Tear down high-resolution resources.
9. Restore the saved baseline sensor descriptor, RGB565 format, two buffers, and
   quality-50 encoder.
10. Discard the measured baseline settling frames.
11. If restoration succeeded, publish the candidate blob; otherwise discard it
    and enter `Unavailable`.
12. Release busy state and record timings/error details.

Every failure after baseline teardown enters the common restoration path.

### 7.2 PhotoStore ownership

`PhotoBlob` is immutable after publication and contains JPEG ownership plus its
metadata. The store owns one reference to its current blob.

- `acquireLatest()` and `acquireById()` lock the store, find the pointer, add a
  reference, and unlock.
- `publish()` assigns the next ID and swaps the pointer under the same mutex.
- The store releases the previous pointer after unlocking.
- HTTP releases its reference after the response finishes or aborts.
- Blob release uses the verified `JpegOutputBuffer` deleter.
- Do not expose the raw store pointer.

The store retains only the latest published photo. A request for any other ID
returns 410, even if an already-running download still holds that old blob.

## 8. Driver and Sensor Requirements

### 8.1 Timed dequeue patch

Patch the pinned component at:

```text
components/espressif__esp_video/src/esp_video_ioctl.c
```

Replace the `portMAX_DELAY` used by `esp_video_ioctl_dqbuf()` with a configurable
tick count. A null result after that wait returns `ESP_ERR_TIMEOUT`; the existing
VFS error mapping converts it to `errno = ETIMEDOUT`.

Do not patch `esp_video_vfs.c` for the queue wait and do not add a separate
watchdog task as a substitute.

### 8.2 Sensor modes

Mode descriptors must match the pinned `esp_cam_sensor_format_t` exactly and
contain complete register tables. Null register arrays are prohibited.

- Preserve the detected baseline descriptor by value for restoration.
- Import the 1920x1080 table only from a version-compatible, licensed source and
  record its source commit.
- Build 640x480 as a separate hardware-validation item. If a direct table is
  unavailable, evaluate the existing 800x640 sensor mode plus a proven ISP
  crop/resize path. Do not label 800x640 or 800x800 as VGA.
- Treat the 2592x1944 table as optional and independently validate its timing,
  exposure, Bayer order, and MIPI line rate.

## 9. Memory Policy

Estimated RGB565 peaks are planning inputs, not acceptance evidence:

| Resource | 1080p estimate | 5 MP estimate |
| --- | ---: | ---: |
| High-res capture buffer | about 4.0 MiB | about 9.6 MiB |
| Retained JPEG capacity | 2 MiB | 4 MiB |
| Baseline buffers/encoder after restore | measured | measured |
| Required reserve after transaction | at least 1 MiB | at least 1 MiB |

Before capture, record total/free PSRAM and largest contiguous PSRAM block.
Reject the request before stopping RTSP if the capture buffer, JPEG capacity,
driver overhead, retained current photo, and 1 MiB reserve cannot fit.

Never silently lower resolution or quality. 5 MP remains disabled if its memory
gate cannot pass on the physical board.

## 10. Phase Plan

### Phase 0: Hardware baseline

Flash the current sketch and record sensor identity, actual 800x800 format, FPS,
JPEG sizes, free/largest PSRAM, capture/encode/send timings, RTSP behavior, and
the VGA probe result.

**Exit:** Hardware evidence is saved and the existing RTSP baseline is stable.

### Phase 1: Reproducible component build

Create the ESP-IDF/Arduino-component application, pin dependency versions, port
the current firmware unchanged, and reproduce Phase 0 behavior.

**Exit:** The component build produces the same camera and RTSP behavior as the
Arduino build and is the documented authoritative build.

### Phase 2: Driver timeout and resource primitives

Patch timed dequeue, implement deterministic timeout injection, add
`JpegOutputBuffer`, and prove repeated engine/buffer cleanup.

**Exit:** A timeout returns within the configured bound, restoration is invoked,
and 100 allocator/encoder lifecycle cycles show no decreasing memory trend.

### Phase 3: CaptureController parity

Move the existing 800x800 capture path behind the controller without adding
high resolution. RTSP must continue to work.

**Exit:** Only the controller owns V4L2, and the baseline passes the Phase 0
stream test.

### Phase 4: 1080p RGB565 transaction

Add the complete 1080p sensor mode, runtime memory gate, settling measurements,
quality-90 encoding, and baseline restoration. Keep the candidate JPEG internal.

**Exit:** Twenty successful mode-switch cycles restore the baseline; injected
capture/encode/allocation failures also restore it; injected restoration failure
enters `Unavailable` and never publishes.

### Phase 5: PhotoStore

Implement immutable blobs, mutex-protected lookup/acquire, atomic release, ID and
boot-generation assignment, and metadata snapshots.

**Exit:** Replacement during five concurrent simulated downloads has no
use-after-free, corruption, leak, or ID mismatch.

### Phase 6: HTTP photo API

Add `esp_http_server`, metadata, conditional JPEG download, and asynchronous
capture control. HTTP must never retain the camera mutex while sending a body.

**Exit:** All status, ETag, disconnect, stale-ID, busy, and unavailable cases
pass. Existing photos remain downloadable while camera state is unavailable.

### Phase 7: RTSP integration and VGA target

Run on-demand capture with connected VLC, ffmpeg, and GStreamer clients. Measure
the media gap and restoration. Validate and enable the 640x480 stream mode.

**Exit:** On-demand capture resumes RTSP without firmware reboot, measured client
behavior is documented, and the release stream is 640x480 or a blocking hardware
limitation is explicitly recorded.

### Phase 8: Background mode and settings

Add the one-entry request queue, minimum-delay scheduler, `Preferences`, settings
API, and a minimal settings page if required for operation.

**Exit:** 100 background attempts never overlap or queue missed intervals,
settings survive reboot, and cadence/memory/RTSP gaps are measured.

### Phase 9: Optional optimizations

Evaluate YUV420 end to end, then evaluate 5 MP only if sensor timing and memory
gates pass. Neither capability blocks the 1080p release.

### Phase 10: Hardening

Run fault, disconnect, restoration, long-duration, and 1000-attempt background
tests. Update README with measured behavior and remove committed credentials.

## 11. Release Acceptance

The core feature is complete when:

- the authoritative component build is reproducible;
- the release RTSP stream is 640x480, quality 50, approximately 10 FPS;
- a bounded dequeue timeout prevents indefinite high-resolution transactions;
- 1920x1080 RGB565 quality-90 JPEGs decode correctly;
- baseline restoration gates publication;
- metadata and JPEG APIs agree on boot generation, ID, and ETag;
- unchanged JPEG requests return 304 without a body;
- current JPEG download remains available when camera capture is unavailable;
- five concurrent downloads are safe during photo replacement;
- background captures never overlap and show no memory trend;
- all required measurements and limitations are documented.

YUV420 and 5 MP are optional and do not block this release.

## 12. Out of Scope

- simultaneous independent VGA and high-resolution sensor output;
- continuous high-resolution streaming;
- more than one retained photo version;
- runtime JPEG quality controls;
- unauthenticated reset or force-mode HTTP endpoints;
- authentication for an untrusted network;
- UI gallery or marketing-style web interface;
- changing the RTSP transport or packetizer protocol without measured need.
