# OV5647 High-Resolution Still Capture and Photo Refresh Plan (v2.2)

**Last Updated:** 2026-08-18  
**Status:** Phase 0 in progress, pending hardware validation  
**Revision:** v2.2 - Final corrections based on architecture review

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

**Current working baseline (as of 2026-08-18):**
- Stream resolution: 800x800
- Capture format: RGB565
- JPEG quality: 50
- Frame rate: 10 FPS
- Capture buffers: 2
- RTSP port: TCP 554
- RTP/JPEG port: UDP 5430

**Target VGA baseline for this plan:**
- Stream resolution: 640x480 VGA
- Capture format: RGB565
- JPEG quality: 50
- Frame rate: 10 FPS
- Capture buffers: 2

**Note:** The transition from 800x800 to 640x480 VGA is part of Phase 1 work,
not the current baseline. Phase 0 must validate the 800x800 baseline on
hardware, then Phase 1 will implement the VGA sensor mode.

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

**Primary implementation:** RGB565 capture format with JPEG quality 90.

| API value | Resolution | Sensor output | Capture format | JPEG quality | Max JPEG |
| --- | ---: | --- | --- | ---: | ---: |
| `1080p` | 1920x1080 | RAW10, up to 30 FPS | RGB565 | 90 | 2 MiB |
| `5mp` | 2592x1944 | RAW10, up to 15 FPS | RGB565 | 90 | 4 MiB |

RGB565 is the first implementation path. YUV420 is an optional optimization
attempted only after RGB565 capture, JPEG encoding, and the retained-photo API
are validated. If YUV420 is validated end-to-end (V4L2 capture + JPEG encoder
support), capture buffer size reduces from width×height×2 to width×height×1.5.

Only one high-resolution capture buffer is allocated at a time.

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
  "boot_generation": "0x123abc456def7890",
  "download_url": "/api/photo/latest.jpg?id=42",
  "width": 1920,
  "height": 1080,
  "bytes": 245760,
  "resolution": "1080p",
  "jpeg_quality": 90,
  "capture_duration_ms": 842,
  "captured_at_ms": 123456789,
  "capture_mode": "on_demand"
}
```

**Possible states:**
- `empty`: No photo has been captured yet
- `capturing`: Capture in progress, previous photo (if any) still available
- `ready`: Photo available for download
- `error`: Last capture failed, previous photo (if any) still available
- `camera_unavailable`: Controller in Unavailable state, previous photo still accessible

Before the first photo, return `200` with `"state": "empty"`. During a
refresh, retain the previous ready photo and report `"state": "capturing"`.
After a failed refresh, retain the previous photo and report an error state.

**API Semantics:**

- `photo_id` is monotonic for the current boot and identifies a **successful
  publication** (not an attempted capture). It increments only when a photo is
  published after successful baseline restoration.

- `boot_generation` is a 64-bit random value generated at startup. The `ETag`
  format is `"{boot_generation}-{photo_id}"` (e.g., `"0x123abc-42"`).

- `captured_at_ms` is diagnostic metadata only, not a freshness key. Clients use
  `photo_id` and `ETag` for version identification, not timestamps.

- When `state` is `capturing`, `error`, or `camera_unavailable`, the metadata
  still includes the current photo's ID and ETag (which haven't changed).

### Settings and capture control

The settings page is served from `/` and uses these supporting routes:

```text
GET /api/settings
PUT /api/settings
POST /api/photo/capture
```

**`POST /api/photo/capture` behavior:**

Queues an explicit capture and returns immediately (does not block for capture
completion):

```text
POST /api/photo/capture
Content-Type: application/json

{
  "resolution": "1080p"
}

Response:
HTTP/1.1 202 Accepted
Content-Type: application/json

{
  "status": "queued",
  "message": "Capture request queued"
}
```

**Error responses:**

- `409 Conflict`: Capture already in progress
- `503 Service Unavailable`: Camera unavailable (restoration failed)
- `400 Bad Request`: Invalid resolution parameter

Clients poll `/api/photo/metadata` to detect completion. The `photo_id` will
increment when the new photo is published.

**`GET /api/photo/latest.jpg?id=<photo_id>` behavior:**

- **No `id` parameter:** Returns current latest photo
- **With `id` parameter:** Returns that specific photo if still retained, or:
  - `410 Gone`: Photo was released (too old, no longer in memory)
  - `503 Service Unavailable`: Camera unavailable, but previous photo still accessible

**Conditional requests:**

```text
GET /api/photo/latest.jpg
If-None-Match: "0x123abc-42"

Response if unchanged:
HTTP/1.1 304 Not Modified
ETag: "0x123abc-42"
```

`PUT /api/settings` validates and persists capture mode, interval, resolution,
and any RTSP policy. These routes are supporting controls and are separate from
the two core photo resources.

## Camera Pipeline

Add a repository-local `CaptureController` that is the single owner of the camera
pipeline. It owns the capture file descriptor, MMAP buffers, V4L2 sensor format
and capture format state, sensor descriptors, encoder instances, and capture
timeout enforcement. `ESPVideoClass` remains responsible for initializing the
ESP32-P4 MIPI-CSI hardware, but all V4L2 ioctls are issued by the controller.

### CaptureController State Machine

The controller implements an explicit state machine with defined transitions and
failure paths:

**States:**
- **Uninitialized:** No fd, no buffers
- **BaselineRunning:** VGA streaming active (RTSP or HTTP MJPEG)
- **Stopping:** Stopping capture, buffers not yet released
- **SensorConfiguring:** Applying new sensor format for high-res
- **BufferAllocating:** Requesting/mapping new MMAP buffers
- **HighResReady:** High-res mode configured, ready to capture
- **Capturing:** Dequeuing high-res frame with timeout
- **Restoring:** Reverting to baseline sensor mode
- **Unavailable:** Failed restoration, cannot resume streaming

**State Transitions:**
```
BaselineRunning → Stopping [on high-res capture request]
Stopping → SensorConfiguring [after VIDIOC_STREAMOFF + munmap old buffers]
SensorConfiguring → BufferAllocating [VIDIOC_S_SENSOR_FMT + VIDIOC_S_FMT succeeded]
BufferAllocating → HighResReady [VIDIOC_REQBUFS + mmap succeeded]
HighResReady → Capturing [VIDIOC_QBUF + VIDIOC_STREAMON succeeded]
Capturing → Restoring [VIDIOC_DQBUF succeeded, frame encoded]
Capturing → Unavailable [timeout/error + restoration failed]
Restoring → BaselineRunning [baseline restored successfully]
Restoring → Unavailable [restoration failed]
Unavailable → Uninitialized [controller reset or reboot only]
```

**Failure Handling:**
- SensorConfiguring fails → attempt restore → BaselineRunning or Unavailable
- BufferAllocating fails → attempt restore → BaselineRunning or Unavailable
- Capturing timeout → attempt restore → BaselineRunning or Unavailable
- Restoring fails → Unavailable (do NOT publish new photo)

**Publication Policy:**
A captured photo is published to PhotoStore ONLY after successful restoration
to baseline mode. If restoration fails, the new photo is discarded, the camera
enters Unavailable state, and the previous photo remains accessible. HTTP
endpoints return `503 Service Unavailable` for capture requests and
`camera_unavailable: true` in metadata when in Unavailable state.

### CaptureController API

The controller must provide:
- Read and retain the driver-selected baseline sensor mode during startup
- Stop capture and release all MMAP buffers
- Apply an `esp_cam_sensor_format_t` with `VIDIOC_S_SENSOR_FMT` while stopped
- Reopen capture with one or two buffers and select RGB565 or YUV420
- Request and map new buffers for the selected format and dimensions
- Capture with bounded dequeue timeout (requires ESP-Video patching)
- Restore baseline mode atomically
- Clean up safely after every partial initialization failure

RTSP streaming and HTTP still capture request frames through the controller API.
No other code may call V4L2 ioctls directly or assume the sensor is in a
particular mode.

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

### Capture timeout implementation

The installed `ESPVideoCaptureDevClass::captureBuffer()` uses a blocking
`VIDIOC_DQBUF`, and the ESP-Video VFS registration has no `select()`/`poll()`
hooks. A software watchdog task can detect elapsed time but **cannot interrupt**
the blocking dequeue call.

**Tier 1: Full Support (Required)**

High-resolution capture requires bounded timeout with recovery. This mandates:

1. Locate the actual ESP-Video component source (not just Arduino library wrapper)
2. Vendor or rebuild the component to patch the VFS ioctl handler
3. Replace `portMAX_DELAY` with finite FreeRTOS wait (e.g., 5000ms)
4. Timeout returns error to CaptureController
5. Controller executes restoration sequence
6. Validate with deterministic fault injection at controller boundary

**Implementation:** Phase 1.5 is a blocking gate. The execution guide must document
the exact ESP-Video component path, patch location, and build integration before
proceeding to Phase 2.

**Tier 2: Degraded Mode (Fallback if Tier 1 Impossible)**

If ESP-Video source is unavailable or patching fails:

1. Document that capture can hang indefinitely on sensor failure
2. High-resolution capture is disabled or marked as "may require reboot on error"
3. Consider this a product limitation, not a supported configuration
4. Rely on external watchdog timer for whole-system reset

**A watchdog task that only observes elapsed time is NOT a timeout implementation
and does NOT provide recovery capability.**

Phase 1.5 must select exactly one tier and document the decision before Phase 2.

## JPEG Encoder and Photo Store

### JPEG Allocator Verification

Phase 1.5 must verify the JPEG memory allocator before any cleanup code is written.
Check the ESP32-P4 Arduino core implementation of `jpeg_alloc_encoder_mem()`:

- If it uses `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`, pair with `free()`
- If it uses a custom allocator, identify the matching deallocator

Create a verified adapter to encapsulate allocation and deallocation:

```cpp
// jpeg_buffer_adapter.h
class JpegOutputBuffer {
private:
  uint8_t* data_;
  size_t capacity_;

public:
  static JpegOutputBuffer* allocate(size_t requested);
  void release();  // Calls verified deallocator
  uint8_t* data() { return data_; }
  size_t capacity() const { return capacity_; }
};
```

No code should call an unverified deallocator directly.

### Encoder Lifecycle

The current `JpegEncoderClass` has no cleanup capability. Phase 2 must add
`end()` method using the verified adapter.

Phase 3 extends the encoder so it can be stopped and recreated for a new
resolution, input format, quality, output capacity, and timeout.

**Primary implementation path:** RGB565 input format. The still encoder must
accept RGB565, generate quality-90 JPEG, enforce the 2 MiB/4 MiB limits, and
validate SOI/EOI markers.

Completed still buffers must be detachable from the encoder before the streaming
encoder is recreated. The encoder output buffer becomes the retained photo
through ownership transfer - no copy is made.

### PhotoStore with Thread-Safe Acquisition

Add a reference-counted immutable photo store using explicit `PhotoBlob` with
manual atomic reference counting and mutex-protected acquisition:

```cpp
struct PhotoBlob {
  uint8_t* jpeg_data;          // From encoder, transferred ownership
  size_t jpeg_size;
  uint32_t photo_id;
  uint32_t width;
  uint32_t height;
  uint32_t capture_time_ms;
  uint64_t boot_generation;
  std::atomic<int> ref_count;
  
  PhotoBlob() : ref_count(1) {}  // Starts with 1 reference (creator)
  
  void release() {
    if (ref_count.fetch_sub(1, std::memory_order_release) == 1) {
      // Last reference released
      JpegOutputBuffer::deallocate(jpeg_data);  // Use verified adapter
      delete this;
    }
  }
};

class PhotoStore {
private:
  PhotoBlob* current_;
  SemaphoreHandle_t mutex_;
  uint32_t next_photo_id_;
  uint64_t boot_generation_;

public:
  // Thread-safe: acquires reference while holding mutex
  PhotoBlob* acquire_latest() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    PhotoBlob* result = current_;
    if (result) {
      result->ref_count.fetch_add(1, std::memory_order_acquire);
    }
    xSemaphoreGive(mutex_);
    return result;
  }

  // Thread-safe: swaps pointer while holding mutex
  void publish(PhotoBlob* new_blob) {
    new_blob->photo_id = next_photo_id_++;
    new_blob->boot_generation = boot_generation_;
    
    xSemaphoreTake(mutex_, portMAX_DELAY);
    PhotoBlob* old = current_;
    current_ = new_blob;
    xSemaphoreGive(mutex_);
    
    if (old) {
      old->release();  // May or may not free immediately
    }
  }
};
```

**Critical race prevention:** The mutex protects pointer lookup and reference
increment as an atomic operation. The atomic counter protects concurrent release
operations. HTTP handlers must call `acquire_latest()`, never read the pointer
directly.

Store behavior:
- Capture allocates and prepares a new JPEG before publishing it.
- Publication atomically swaps the latest photo and increments `photo_id`.
- HTTP handlers acquire a reference before sending the body.
- The previous photo is freed only after all active downloads release it.
- Allocation or encoding failure preserves the previous photo and ID.
- Metadata and download responses refer to the same photo version.

Log total PSRAM, free PSRAM, largest free block, retained-photo size, and
encoder/capture timings before and after every transaction. Reserve at least
1 MiB after accounting for capture buffers, JPEG capacity, driver overhead,
and the retained-photo store.

### Memory Budget

Peak PSRAM usage occurs when VGA encoder is recreated while still holding the
detached high-resolution JPEG. The encoder output buffer IS the retained photo
through ownership transfer - they are the same allocation, not separate.

| Component | 1080p Peak | 5MP Peak |
|-----------|------------|----------|
| VGA capture buffers (2×) | 800×800×2×2 = 2.4 MiB | 2.4 MiB |
| VGA JPEG output buffer | ~0.8 MiB | ~0.8 MiB |
| High-res capture buffer (1×) RGB565 | 1920×1080×2 = 4.0 MiB | 2592×1944×2 = 9.8 MiB |
| High-res JPEG output (retained) | 2 MiB | 4 MiB |
| Driver overhead | ~1 MiB | ~1 MiB |
| **Total peak** | **~10.2 MiB** | **~18.0 MiB** |
| **Required PSRAM** | **≥12 MiB** | **≥20 MiB** |

**Note:** If YUV420 is validated end-to-end, high-res capture buffer reduces to:
- 1080p: 1920×1080×1.5 = 3.0 MiB (saves 1.0 MiB)
- 5MP: 2592×1944×1.5 = 7.3 MiB (saves 2.5 MiB)

**If board has 16 MiB PSRAM:** 5MP mode will likely fail due to insufficient
memory. Options:
1. Reduce baseline stream to 640×480 VGA (saves ~1.5 MiB)
2. Limit to 1080p maximum resolution
3. Validate YUV420 support to reduce high-res buffer size

Do not silently reduce resolution or quality when memory is insufficient.
Report allocation failures with error state and preserve the previous photo.

## Capture Transaction

The HTTP handlers, background scheduler, and RTSP loop share the camera and
hardware JPEG engine. Serialize them with a camera mutex and an atomic busy
flag.

For every capture:

1. Claim the operation; return `409` for an overlapping explicit request.
2. Lock the pipeline and make the RTSP loop skip frame capture.
3. Stop VGA capture and release its two buffers and streaming encoder.
4. Apply the requested high-resolution sensor mode.
5. Start one-buffer YUV420 (or RGB565) capture and the quality-90 encoder.
6. Discard settling frames (mode-specific count, see below).
7. Capture and encode the next frame; detach the JPEG buffer.
8. Stop high-resolution capture and release high-resolution resources.
9. Restore the saved VGA mode, two RGB565 buffers, and quality-50 encoder.
10. Discard VGA settling frames (5 frames, ~500ms at 10 FPS).
11. Publish the detached JPEG to `PhotoStore`.
12. Unlock the pipeline and resume RTSP media.

### Settling frame counts

After mode change, discard the first N frames before capturing:

| Transition | Settling frames | Estimated time |
|------------|-----------------|----------------|
| VGA → 1080p | 5 frames | ~167 ms @ 30 FPS |
| VGA → 5MP | 7 frames | ~467 ms @ 15 FPS |
| High-res → VGA | 5 frames | ~500 ms @ 10 FPS |

Every failure after step 3 uses the same restoration path. For an explicit
capture request, a restoration failure returns `500`. For a scheduled
capture, it records the failure in metadata. In both cases it marks the camera
unavailable, keeps the HTTP service alive, and logs the exact failed
operation.

### RTSP session keepalive

During the capture transaction (steps 3-12), the RTSP control session must
remain alive. Expected transaction duration:

- 1080p: ~1.5 seconds (settling + capture + restore)
- 5MP: ~2.0 seconds (longer settling time)

**Approach:** Measure actual RTSP client behavior during mode switching rather
than assuming any single keepalive mechanism is universally sufficient.

**Phase 5 measurement requirements:**

1. Implement on-demand capture without RTSP-specific keepalive code
2. Test with common clients:
   - VLC: Does it reconnect automatically? Does it timeout?
   - ffmpeg: Does it buffer through the gap? Does it report errors?
   - GStreamer: Does it maintain the session? Does it require configuration?
3. Document measured media gap duration (expected ~1.5s for 1080p)
4. **Only add keepalive mechanism if measurement shows it's necessary**
5. If keepalive is needed, choose based on which clients fail (RTCP sender reports, RTSP OPTIONS, or other)

**Acceptance criteria:**

- **On-demand mode:** RTSP control session survives capture transaction. Media
  resumes automatically after baseline restoration. Measured gap duration is
  documented. Clients successfully reconnect or buffer through the gap.

- **Background mode:** May tolerate measured media gaps, may disable RTSP media
  during background capture, or may reduce capture frequency to maintain
  acceptable media continuity. Background capture at 1-second intervals is
  expected to cause continuous gaps since the sensor pipeline is shared.

The application should preserve the RTSP control session and document measured
client-specific behavior rather than treating one keepalive mechanism as
universally sufficient.

## Implementation Phases

Each phase has one primary risk and must pass its exit condition before the
next phase starts.

### Phase 0: Baseline, feasibility, and diagnostics

**Current status:** Code complete, pending hardware validation.

- Reconcile the current working-tree stream quality and interval with the
  documented target.
- Record actual stream dimensions, FPS, JPEG size, PSRAM use, and timings.
- Confirm sensor PID `0x5647`.
- Determine whether direct 640x480 output is available or whether a local
  sensor mode or measured resize fallback is required.
- Validate that diagnostic logging confirms 800x800 baseline.

**Exit condition:** The 800x800 baseline is validated on hardware with serial
logs showing actual FPS, JPEG sizes, PSRAM usage, and sensor identity. The
blocking dequeue behavior is confirmed on hardware.

**Implementation status (2026-08-18):** The firmware now uses the selected
quality-50, 10 FPS target; logs heap/PSRAM and active formats; confirms the
OV5647 through ESP-Video's register-based auto-detection; probes VGA format
readback; reports the blocking-dequeue limitation; and emits five-second
rolling FPS, JPEG-size, capture, encode, send, and drop measurements. Normal
and camera-only ESP32-P4 builds pass with Arduino core 3.3.11. Phase 0 remains
open until the firmware is flashed to the physical board and the serial/RTSP
measurements and VGA probe result are recorded.

### Phase 1: Format support validation

**Risk:** YUV420 or 640x480 VGA modes may not be available.

- Probe V4L2 device for `V4L2_PIX_FMT_YUV420` support via `VIDIOC_ENUM_FMT`.
- If YUV420 unsupported, document RGB565 fallback and adjust Phase 3 plan.
- Add the direct 640x480 VGA mode descriptor or validate 800x640→640x480 resize.
- Add the repository-local 1920x1080 RAW10 descriptor and register table.
- Add a camera-only diagnostic that reads back applied sensor modes.

**Exit condition:** Format support is documented. If YUV420 is unavailable,
the plan is updated to use RGB565 for high-res with adjusted memory budgets.
If 640x480 VGA is unavailable, the plan accepts 800x640 cropped to 640x480 or
retains 800x800 as the streaming baseline.

### Phase 1.5: Timeout mechanism gate (BLOCKING)

**Risk:** Cannot implement bounded capture timeout, making recovery from sensor
hangs impossible.

- Implement finite dequeue timeout via ESP-Video vendoring/patching.
- OR implement watchdog task that monitors capture duration and can reset
  camera pipeline.
- Demonstrate recovery from simulated 5-second sensor hang.
- Document the chosen timeout strategy and recovery mechanism.

**Exit condition:** Repeated simulated hangs are detected and recovered within
10 seconds without requiring board reboot. This phase BLOCKS all subsequent
work - if neither approach succeeds, the transaction design must be revised.

### Phase 2: Encoder lifecycle and mode switching

**Risk:** Encoder cleanup may leak resources.

- Add `JpegEncoderClass::end()` with proper cleanup of handle and output buffer.
- Verify repeated `begin()`/`end()` cycles don't leak memory (100 iterations).
- Add the V4L2 capture controller with stream stop/start, MMAP ownership,
  buffer-count changes, and bounded capture.
- Implement `800x800 -> 1080p -> capture -> 800x800` (or VGA if Phase 1
  completed VGA mode).
- Keep high-resolution JPEG delivery out of RTSP path.

**Exit condition:** Repeated encoder recreation shows no PSRAM reduction over
100 cycles. Repeated mode switches produce valid 1920x1080 buffers and restore
streaming baseline without rebooting.

### Phase 3: JPEG lifecycle and retained photo store

**Risk:** Reference counting bugs or memory fragmentation.

- Extend encoder to support runtime reconfiguration: input format (RGB565 or
  YUV420), quality, output capacity, timeout.
- Add output buffer ownership transfer (detach capability).
- Implement `PhotoStore` with `std::shared_ptr<PhotoBuffer>` or atomic
  reference counting.
- Add photo IDs, boot generation, ETags, and metadata snapshots.
- Test concurrent access: 5 simultaneous downloads while swapping photo.

**Exit condition:** Known YUV420 (or RGB565) data produces valid JPEG.
Repeated encoder and store replacement over 100 cycles does not reduce free
PSRAM. Concurrent downloads complete successfully without corruption.

### Phase 4: Standalone HTTP service

**Risk:** HTTP server integration issues or memory contention.

- Start `esp_http_server` on port 80.
- Implement conditional metadata endpoint with `If-None-Match`/`304` support.
- Implement JPEG download endpoint with ETag and `X-Photo-Id` headers.
- Implement settings GET/PUT and explicit-capture POST routes.
- Test all states: empty, capturing, ready, error, camera-unavailable.

**Exit condition:** Clients can poll metadata, receive `304` when unchanged,
download the matching JPEG by ID, handle empty/capturing/error states, and
receive appropriate status codes (404, 410, 409, 500, 503, 504).

### Phase 5: RTSP and photo integration

**Risk:** Camera mutex deadlock or RTSP session drops.

- Add camera mutex and atomic busy state.
- Make RTSP loop skip capture when busy flag is set.
- Implement full capture transaction (steps 1-12).
- Restore streaming baseline before publishing the photo.
- Add RTCP keepalive or RTSP OPTIONS responses during capture.
- Verify connected RTSP sessions resume after successful and failed captures.

**Exit condition:** On-demand capture works with and without an active RTSP
client and does not require RTSP reconnect. RTSP sessions survive 10
consecutive captures without timeout.

### Phase 6: NVS settings and background capture

**Risk:** Settings corruption or background scheduler overlap.

- Persist `capture_mode`, `interval_ms`, and resolution through `Preferences`.
- Add the settings page HTML/CSS/JavaScript.
- Add runtime mode changes that apply at transaction boundaries.
- Add the background scheduler with one-second default interval.
- Measure capture cadence, memory stability, and RTSP media gaps.

**Background scheduling behavior:** Interval is measured from completion of the
previous capture transaction to the start of the next. Captures never overlap.
Missed intervals are not queued. A 1-second interval does not guarantee
one-second spacing - actual cadence will be interval + transaction duration
(~1.5-2.0 seconds per capture).

**Exit condition:** Settings changes persist across reboots and apply at
transaction boundaries. 100 background captures complete without resource
growth or overlapping captures. Measured cadence matches interval + transaction
duration (not interval alone).

### Phase 7: Full 5 MP sensor mode

**Risk:** Insufficient PSRAM or register table errors.

- Add and validate the 2592x1944 RAW10 register profile.
- Verify peak memory usage against available PSRAM.
- Enforce the 4 MiB JPEG limit and PSRAM reserve.
- Make `5mp` the default only after physical validation.

**Exit condition:** The API returns decodable 2592x1944 quality-90 JPEGs. The
measured memory margin is at least 1 MiB. If PSRAM is insufficient, document
the limitation and retain 1080p as maximum.

### Phase 8: Final hardening and documentation

**Risk:** Untested edge cases in production.

- Exercise every documented error, timeout, disconnect, and restoration path.
- Run 100 captures per mode (on_demand and background, 1080p and 5MP).
- Stress test: 1000 background captures at 1-second interval.
- Document measured VGA behavior, photo latency, memory margins, and RTSP gaps
  in the README.
- Remove or sanitize Wi-Fi credentials from committed code.

**Exit condition:** All error paths have been triggered and logged. Long-term
stability is demonstrated. README accurately reflects measured behavior.

## Test Plan

### Build and static checks

- Compile normal Wi-Fi/RTSP and `EXCLUDE_WIFI` builds.
- Confirm the packetizer accepts 640x480 and rejects accidental high-resolution
  input from the stream path.
- Add host tests for metadata JSON, ETag generation, ID matching, and
  reference-counted photo replacement.

### Functional tests

- Verify decoded RTSP frames are at the selected baseline dimensions and FPS.
- Apply every sensor descriptor and verify readback with `VIDIOC_G_SENSOR_FMT`.
- Switch repeatedly between baseline and 1080p, then restore baseline (20 cycles).
- Encode known YUV420 (or RGB565) data and validate JPEG markers and dimensions.
- Poll metadata and verify `photo_id` changes only after successful publish.
- Verify unchanged ETags return `304` with no body.
- Verify the metadata URL and JPEG body identify the same photo.
- Verify stale requested IDs return `410 Gone`.
- Verify empty, capturing, failed-refresh, and camera-unavailable states.
- Verify orientation, color order, and field of view with a color chart.

### Streaming and reliability tests

- Keep an RTSP receiver connected while repeating on-demand captures (20 cycles).
- Run background capture at one-second intervals and measure actual spacing
  between capture starts (interval + transaction duration, not interval alone).
- Record media gap duration separately for on-demand and background modes.
- Run at least 100 captures without rebooting for each still resolution.
- Confirm PSRAM, MMAP buffers, encoder handles, and retained blobs do not trend
  downward or grow over 100 captures.
- Disconnect HTTP clients during JPEG transfer and verify reference cleanup.

### Failure tests

- Unsupported resolution: `400 Bad Request`.
- Overlapping capture: `409 Conflict`.
- No photo yet: `404 Not Found`.
- Requested old photo released: `410 Gone`.
- Allocation failure: `503 Service Unavailable`, with baseline restoration.
- Capture or encoding timeout: `504 Gateway Timeout`, with baseline restoration.
- Sensor-mode failure: restore saved baseline mode.
- Restoration failure: `500 Internal Server Error`, camera marked unavailable,
  HTTP service remains alive.

## Acceptance Criteria

- The RTSP stream produces compatible frames at the documented baseline
  (800x800 or 640x480 depending on Phase 1 outcome).
- The API returns decodable 1920x1080 and (if PSRAM sufficient) 2592x1944
  quality-90 photos.
- Metadata polling reliably identifies new photos without retransmitting old
  JPEGs.
- Conditional downloads return `304` for an unchanged photo.
- The previous photo remains available while a refresh is running or fails.
- On-demand capture preserves the RTSP session and restores baseline streaming.
- Background capture runs at measured spacing of interval + transaction duration
  (not interval alone) without resource leaks.
- All documented memory, timeout, busy, and restoration errors are observable.
- Peak memory usage stays within available PSRAM with at least 1 MiB margin.

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

## Revision History

**v2.2** (2026-08-18) - Final architecture corrections
- Changed timeout to two-tier system: Tier 1 (ESP-Video patching required) or Tier 2 (degraded mode)
- Fixed format sequence: RGB565 as primary path, YUV420 as optional optimization
- Added PhotoStore mutex-protected acquire() to prevent use-after-free race
- Added JPEG allocator verification requirement with adapter pattern
- Added complete CaptureController state machine with explicit transitions
- Added publication-on-failure policy: do NOT publish if restoration fails
- Clarified API semantics: photo_id increments only on successful publication
- Updated RTSP requirements to measurement-based approach
- Added measurable memory gates with runtime validation
- Fixed background cadence wording: interval + transaction duration, not interval alone
- Based on final architecture review feedback

**v2.1** (2026-08-18)
- Simplified boot generation (removed MAC address)
- Clarified camera pipeline unified ownership model
- Updated timeout mechanism to emphasize ESP-Video patching requirement
- Changed primary implementation path to RGB565, YUV420 as optimization
- Updated PhotoStore to use explicit PhotoBlob with manual refcounting
- Clarified memory budget: encoder output IS retained photo (transfer ownership)
- Updated RTSP keepalive to emphasize measurement over prescription
- Based on implementation review feedback

**v2.0** (2026-08-18)
- Initial version with corrections from original plan
- Added explicit memory budget calculations
- Clarified Phase 0 baseline (800x800 current, 640x480 target)
- Added timeout mechanism as blocking gate
- Expanded JPEG encoder lifecycle requirements

