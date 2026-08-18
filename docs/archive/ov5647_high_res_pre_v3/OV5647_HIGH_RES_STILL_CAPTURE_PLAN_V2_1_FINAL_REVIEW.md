# OV5647 High-Resolution Still Capture Plan V2.1
# Final Review Before Execution-Guide Revision

**Reviewed:** 2026-08-18  
**Inputs:**

- `IMPLEMENTATION_REVIEW_RESPONSE.md`
- `OV5647_HIGH_RES_STILL_CAPTURE_PLAN_V2.md` (revision v2.1)
- Current ESP32-P4 Arduino/ESP-Video repository state

## Review Scope

This document is a final architecture review. The attached plans are treated
as proposals, not as execution instructions. No firmware implementation is
authorized or performed by this review.

## Decision

V2.1 is substantially improved and has the right overall architecture. The
retained-photo API, unified camera ownership, RGB565-first strategy, and
explicit memory discussion are all correct directions.

The plan is **not yet ready for the execution guide to be followed literally**.
There are a few remaining contradictions and one important PhotoStore race. The
execution guide should be revised only after the mandatory decisions below are
incorporated into V2.1.

## Resolved Issues

The following review findings have been addressed at the architectural level:

- The watchdog is acknowledged as unable to interrupt a blocking dequeue.
- Camera ownership is assigned to one repository-local controller.
- The HTTP design uses `esp_http_server` and retained-photo resources.
- Boot generation is now a per-boot random value rather than a MAC/time mix.
- RGB565 is identified as the first high-resolution implementation path.
- Encoder output is intended to transfer directly into the retained photo.
- RTSP keepalive behavior is to be measured against actual clients.
- Gallery, quality controls, and reset routes are no longer part of the core
  execution path.

These are meaningful corrections. The remaining work is to make the written
contract internally consistent and implementable.

## Mandatory Changes Before Execution

### 1. Make Timeout Policy Unambiguous

The plan correctly requires patched or rebuilt ESP-Video for a real bounded
dequeue, but Phase 1.5 still presents a watchdog as an alternative and still
requires recovery without reboot.

The plan must choose exactly one of these states:

**Supported state:**

- The actual ESP-Video component is vendored or rebuilt.
- `VIDIOC_DQBUF` uses a finite FreeRTOS wait.
- Timeout returns an error to the capture controller.
- Recovery is demonstrated using deterministic fault injection.

**Unsupported state:**

- Bounded dequeue is unavailable.
- Capture can hang until a watchdog reset or manual reboot.
- High-resolution mode switching is blocked, or the limitation is explicitly
  accepted as a product constraint.

A watchdog task that only observes elapsed time must not be described as a
timeout implementation or recovery mechanism.

### 2. Define the Actual Format Sequence

V2.1 still declares YUV420 in the high-resolution behavior table while later
sections describe RGB565 as the primary implementation path.

The plan should state this sequence explicitly:

1. VGA stream: current 800x800 baseline until hardware validation; 640x480 is
   a later target.
2. First still implementation: 1920x1080 RGB565 input, quality 90.
3. Retained-photo API: RGB565 implementation only.
4. Optional optimization: validate YUV420 capture and JPEG encoding separately.
5. 5 MP: only after memory and sensor-mode validation.

YUV420 must not appear as a required Phase 2 or Phase 3 capability.

### 3. Fix PhotoStore Acquisition Atomicity

An atomic reference counter alone does not make this safe:

```text
download reads latest pointer
publisher releases old blob
download increments old blob reference count
```

The download thread can increment a freed object. The plan must define an
atomic “lookup plus acquire” operation, for example:

- lock the store while reading the current pointer and incrementing its count;
- keep the lock until the HTTP handler owns the reference; or
- use a proven atomic shared-ownership primitive.

The simplest embedded implementation is a mutex-protected `PhotoStore` with
manual reference counting. The atomic counter protects release operations; the
store mutex protects pointer lookup and acquisition.

The plan must also define allocation ownership for `PhotoBlob` itself. If the
JPEG bytes use an ESP JPEG allocator but the `PhotoBlob` object uses `new`,
their release paths must be separate and explicit.

### 4. Replace Placeholder JPEG Cleanup With a Verified Adapter

The plan currently mentions both `jpeg_free_encoder_mem()` and `free()` while
the installed encoder uses `jpeg_alloc_encoder_mem()`.

Before the execution guide includes cleanup code, add a small verified adapter,
for example:

```cpp
class JpegOutputBuffer {
public:
  static JpegOutputBuffer allocate(size_t requested);
  void release();
};
```

The adapter must be implemented against the exact installed ESP32-P4 core API.
No sample should call an unverified deallocator directly.

### 5. Specify CaptureController State Transitions

The unified controller is the correct abstraction, but its state machine must be
written down before implementation. At minimum, define:

```text
BaselineRunning
Stopping
SensorModeApplying
BuffersAllocating
HighResRunning
Capturing
RestoringBaseline
Unavailable
```

Every transition needs a failure destination. In particular:

- old MMAP buffers must be released before dimensions change;
- new buffers must be requested and mapped after the format changes;
- only the controller may own the video fd;
- a failed restoration must prevent RTSP capture from resuming;
- a captured JPEG must not be published until the required restoration policy
  is satisfied.

The execution guide should not create a separate `ModeManager` that bypasses
this controller.

### 6. Define Publication Behavior When Restoration Fails

The capture transaction publishes after restoring VGA, but the failure policy
needs to be explicit.

Recommended rule:

- If capture or encoding fails: discard the new blob and keep the previous photo.
- If restoration fails: do not publish the new blob as the normal latest photo;
  mark the camera unavailable and retain the previous published photo.
- The HTTP service remains available and reports `camera_unavailable`.

This avoids advertising a new photo while the camera is left in an unknown mode.

### 7. Keep API Semantics Focused on Version IDs and ETags

The current two-resource design is appropriate:

```text
GET /api/photo/metadata
GET /api/photo/latest.jpg?id=<photo_id>
```

The plan should preserve these rules:

- `photo_id` identifies a successful publication, not an attempted capture.
- `ETag` identifies the boot generation and photo version.
- Metadata returns `capturing` or `error` even when the old photo ETag is
  unchanged.
- A specific old ID returns `410 Gone` only after that blob is released.
- An omitted ID returns the current latest photo.
- `POST /api/photo/capture` queues work and returns `202`; it does not hold an
  HTTP request open for the whole sensor transaction.

No timestamp-based freshness comparison is required. Capture time remains
diagnostic metadata.

### 8. Correct the RTSP Acceptance Criteria

The plan now correctly says to measure client behavior, but another section
still prescribes adding RTCP or OPTIONS keepalive.

Change the requirement to:

- keep the RTSP control task alive;
- measure VLC, ffmpeg, and GStreamer during the actual media gap;
- add a keepalive only if a measured client requires it;
- document the maximum observed gap for on-demand mode;
- permit background mode to tolerate gaps or disable RTSP media by policy.

The acceptance test must not require continuous VGA media during one-second
background capture because the sensor pipeline is intentionally shared.

### 9. Make Memory Gates Measurable

The memory calculations are much clearer, but the plan should define the gate
using runtime measurements rather than only estimated MiB values.

Before enabling a resolution, record:

- free PSRAM;
- largest free PSRAM block;
- capture-buffer allocation result;
- JPEG output allocation result;
- retained-photo allocation result;
- post-transaction reserve.

If the configured reserve is not available, return `503`, retain the previous
photo, and do not silently lower resolution or quality. 5 MP should remain an
optional capability, not a required milestone for boards where the measured
budget cannot fit.

### 10. Correct Background-Cadence Wording

A one-second interval cannot be guaranteed when a capture transaction takes
longer than one second. The scheduler behavior described elsewhere is sound:
measure from completion, do not queue missed intervals, and never overlap.

The acceptance criterion should therefore say:

> Captures never overlap, the configured interval is a minimum delay between
> completed attempts, and measured cadence is reported.

It should not require that actual cadence exactly equal the configured interval.

## Execution-Guide Requirements

After the plan changes above, the execution guide should be rewritten with these
constraints:

- use the actual ESP-Video component path and build strategy;
- use one `CaptureController`, not independent mode and capture owners;
- use `esp_http_server` only;
- implement RGB565 first;
- postpone YUV420, 5 MP, gallery, and runtime quality controls;
- include host tests for PhotoStore acquisition/replacement races;
- include a verified JPEG allocator adapter;
- make every code snippet compile against the installed headers;
- label conceptual pseudocode explicitly and keep it separate from executable
  instructions;
- do not include unauthenticated reset or force-mode routes.

## Final Readiness Gate

I would consider the plan ready for implementation when all of these are true:

1. Timeout behavior is either genuinely implemented or explicitly removed from
   the supported feature contract.
2. RGB565 is the only required first-path still format.
3. PhotoStore lookup and reference acquisition are race-safe.
4. JPEG allocation and release are verified against the installed core.
5. CaptureController ownership and failure states are specified.
6. Restoration failure and publication behavior are defined.
7. RTSP requirements are measured rather than guessed.
8. Memory and cadence acceptance criteria use measurable gates.

Once these are incorporated, the execution guide can be updated and followed
without another architecture rewrite. At that point, implementation should
begin with Phase 0 hardware validation and the controller/timeout foundation,
not with HTTP handlers or a web UI.
