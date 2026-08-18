# OV5647 High-Resolution Still Capture Execution Guide V3

**Version:** 3.0  
**Status:** Authoritative execution guide; do not skip phase gates  
**Plan:** `OV5647_HIGH_RES_STILL_CAPTURE_IMPLEMENTATION_PLAN_V3.md`  
**Target:** ESP32-P4, Arduino core 3.3.11, ESP-IDF 5.5.x, OV5647 MIPI-CSI

## 0. Execution Rules

This guide is the implementation procedure for Plan V3. The earlier long guide
and its code snippets are archived and must not be followed.

Rules:

1. Read the current source before editing it.
2. Keep the current Arduino sketch build working until the ESP-IDF component
   build passes baseline parity.
3. Do not copy code examples without compiling against the installed headers.
4. Do not invent sensor register arrays, JPEG deallocators, or V4L2 structure
   fields.
5. One phase may be in progress at a time. Record results before continuing.
6. A failed gate blocks dependent phases. Record the blocker; do not silently
   downgrade behavior.
7. Never add an unauthenticated reset or force-mode HTTP route.
8. Do not expose Wi-Fi credentials in new files or result documents.

The implementation is not considered started until Phase 0 hardware evidence and
the Phase 1 build strategy are recorded.

## 1. Source and Toolchain Inventory

Record these values in `docs/v3_toolchain.md` before editing firmware:

```text
Arduino core: 3.3.11
ESP-IDF headers: 5.5.5
ESP-Video component: 0.5.1
ESP-Video source commit: 7c343dc478f73e3234ed898eb358accd8de92ff7
Target: esp32p4
Flash: 16 MiB
PSRAM: board-reported value
```

Useful local paths on the current development machine:

```text
/Users/szemy/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11
/Users/szemy/Workspace/Genican/ESP32P4_video_transmission/reference/17_mipicamera/managed_components
```

Do not make the plan depend on these absolute paths in committed source. Use
project-relative component paths and document the developer-specific toolchain
setup separately.

## 2. Phase 0: Hardware Baseline

**Purpose:** validate the existing firmware before migration.

### Steps

1. Confirm that a physical ESP32-P4 board is connected. Record the port; do not
   guess from a Bluetooth or debug-console port.
2. Build the existing Arduino sketch using the known FQBN and the installed
   Arduino CLI.
3. Flash the board and capture a complete 115200-baud serial log.
4. Run `tools/rtsp_viewer.py` against the board.
5. Record:
   - detected PID and sensor mode;
   - actual width, height, fourcc, and FPS;
   - average and maximum JPEG size;
   - free PSRAM and largest free PSRAM block;
   - capture, encode, send, and drop timings;
   - result of the 640x480 probe;
   - evidence that `VIDIOC_DQBUF` currently blocks.
6. Save the log as `docs/phase0_hardware_baseline.log` and the measurements as
   `docs/phase0_results.md`.

### Exit gate

The 800x800 baseline is stable for at least five minutes and the result file
contains actual measurements. If no board is available, stop here and mark the
phase `blocked: hardware unavailable`; do not claim completion.

## 3. Phase 1: ESP-IDF/Arduino Component Build

**Purpose:** make ESP-Video source-controlled without changing runtime behavior.

### Project shape

Create a new authoritative application under `firmware/`:

```text
firmware/
  CMakeLists.txt
  sdkconfig.defaults
  main/
    CMakeLists.txt
    main.cpp
    src/                 # current RTSP and application sources
  components/
    arduino-esp32/       # pinned Arduino 3.3.11 component
    espressif__esp_video/
    espressif__esp_cam_sensor/
    espressif__esp_sccb_intf/
    espressif__cmake_utilities/
```

Use Arduino as a component with `CONFIG_AUTOSTART_ARDUINO=y` and
`CONFIG_FREERTOS_HZ=1000`. Preserve the current `setup()` and `loop()` during
the first migration. Do not combine the old Arduino static ESP-Video archive
with the source-managed component.

Copy only the version-pinned source components required by ESP-Video from the
repository reference tree. Record the source commit and local modifications in
`firmware/components/espressif__esp_video/PATCHES.md`.

### Migration sequence

1. Port the current sketch and RTSP sources with no feature changes.
2. Configure the same ESP32-P4 pins, partition, Wi-Fi, and PSRAM settings.
3. Build with ESP-IDF.
4. Flash and compare Phase 0 serial metrics and RTSP behavior.
5. Resolve only build or parity issues in this phase; do not add still capture.

### Exit gate

The ESP-IDF/Arduino-component application builds, flashes, initializes the
camera, and reproduces the existing RTSP baseline. If the component build cannot
be made reproducible, stop and document the build blocker. Do not patch the
Arduino precompiled archive as an alternative.

## 4. Phase 2: Timed Dequeue and JPEG Buffer Primitive

This phase has two independent gates. Both must pass before mode switching.

### 4.1 ESP-Video timeout patch

Patch the source-managed component file:

```text
firmware/components/espressif__esp_video/src/esp_video_ioctl.c
```

In `esp_video_ioctl_dqbuf()`:

1. Replace the `portMAX_DELAY` tick value with a component configuration value,
   default 5000 ms.
2. Call `esp_video_recv_element()` with that finite tick count.
3. When no element is returned, return `ESP_ERR_TIMEOUT`, not a generic failure.
4. Confirm the existing VFS error mapping exposes `ETIMEDOUT` to the caller.
5. Keep the patch small and record the upstream commit and reason in
   `PATCHES.md`.

The actual wait is in `esp_video_ioctl.c`; do not place the queue timeout only in
`esp_video_vfs.c`.

### 4.2 Timeout tests

Add a test-only controller fault injection that makes the dequeue operation
return `ETIMEDOUT` after the configured test delay. This verifies controller
restoration without depending on a physically broken sensor.

Then perform a lower-level integration test with a live device and no delivered
frame, if the hardware test setup can safely produce that condition. Record:

- requested timeout;
- measured return time;
- errno/result;
- controller state after return;
- baseline restoration result.

Do not use a task that merely watches elapsed time while another task remains
blocked. A whole-system watchdog reset is a last-resort safety mechanism, not a
capture timeout implementation.

### 4.3 JPEG allocator verification

Inspect the pinned ESP-IDF JPEG header and implementation symbols. The installed
core exposes `jpeg_alloc_encoder_mem()` but no public
`jpeg_free_encoder_mem()`. For this pinned version, validate that the returned
memory is compatible with `free()` before relying on it.

Create `firmware/main/src/jpeg_buffer_adapter.{h,cpp}` with:

- allocation using the existing `jpeg_encode_memory_alloc_cfg_t`;
- capacity returned by the allocator;
- `release()` using the verified `free()` pairing;
- `detach()` transferring the pointer and capacity without copying;
- no double release after detach.

Run 100 allocation/release cycles and 100 encoder create/destroy cycles. Record
free PSRAM and largest block before and after.

### Exit gate

Timed dequeue returns a bounded error and controller fault injection reaches a
restoration path. JPEG memory has a verified release path. If either result is
missing, stop before Phase 3.

## 5. Phase 3: CaptureController Baseline Parity

**Purpose:** make one object the sole V4L2 owner before adding high resolution.

Create:

```text
firmware/main/src/capture_controller.h
firmware/main/src/capture_controller.cpp
```

The controller must own:

- the video fd;
- sensor and output format state;
- MMAP pointers and lengths;
- stream start/stop;
- buffer queue/dequeue;
- baseline and still JPEG encoder lifecycles;
- timeout handling;
- state and error transitions.

Do not create a separate `ModeManager`. Do not let RTSP, HTTP, or the scheduler
call V4L2 ioctls directly.

### Required states

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

### Baseline implementation

Initially implement only the existing 800x800 mode. The controller should:

1. open the video device;
2. read and retain the sensor and output baseline formats;
3. request/map two MMAP buffers;
4. start streaming;
5. return a baseline frame to the existing RTSP packetizer;
6. stop, release, and restart cleanly.

If the existing wrapper hides fd or buffer ownership needed for this contract,
adapt its logic into the controller or add a narrow, documented API. Do not open
a second fd behind the controller.

### Exit gate

The RTSP path uses only controller-provided baseline frames and passes the Phase
0 parity test. Repeated stop/start cycles show no buffer or memory trend.

## 6. Phase 4: 1080p RGB565 Capture

### Sensor descriptor

Create a repository-local descriptor using the exact pinned
`esp_cam_sensor_format_t` structure. Include complete reset, PLL, crop, output,
timing, exposure/gain, and MIPI register data. Record the source commit or
license for every imported table.

Do not compile a descriptor with null registers, guessed fields, or a struct from
another ESP-Video release.

### Transaction

Implement the controller transaction in this order:

1. Verify `BaselineRunning` and acquire the camera operation.
2. Run the runtime memory gate before stopping baseline.
3. Stop stream and release baseline buffers/encoder resources.
4. Apply the complete 1080p sensor format.
5. Set RGB565 output and request/map one capture buffer.
6. Start stream and discard measured settling frames.
7. Dequeue with the patched timeout and encode quality 90.
8. Validate JPEG SOI/EOI, dimensions, and maximum size.
9. Tear down high-resolution buffers and encoder.
10. Restore the saved baseline mode, two buffers, and quality-50 encoder.
11. Discard measured baseline settling frames.
12. Publish the candidate only after restoration succeeds.

On capture, encode, allocation, or timeout failure, run restoration. On
restoration failure, discard the candidate, enter `Unavailable`, and retain the
previous photo.

### Exit gate

Twenty successful 1080p transactions restore 800x800 without reboot. Injected
capture, encoding, allocation, and restoration failures reach the documented
states. No high-resolution JPEG reaches RTSP.

## 7. Phase 5: PhotoStore

Create:

```text
firmware/main/src/photo_store.h
firmware/main/src/photo_store.cpp
```

Implement an immutable `PhotoBlob` and a mutex-protected `PhotoStore`.

Required operations:

- `acquireLatest()`;
- `acquireById(photo_id)`;
- `publish(candidate)`;
- `release(blob)` through the JPEG adapter;
- metadata snapshot creation.

`acquireLatest()` and `acquireById()` must find the pointer and increment its
reference count while holding the store mutex. Never expose `current_` directly.
The store owns one reference; each HTTP sender owns one reference; the creator
transfers its reference to the store on publication.

### Exit gate

Run five concurrent download simulations while publishing replacements. Verify
that every body matches its metadata ID, no freed memory is accessed, and free
PSRAM does not trend downward over 100 replacements.

## 8. Phase 6: HTTP Photo API

Use ESP-IDF `esp_http_server` only. Create:

```text
firmware/main/src/photo_api.h
firmware/main/src/photo_api.cpp
```

Start the server after Wi-Fi and camera initialization. The handlers are:

- `GET /api/photo/metadata`;
- `GET /api/photo/latest.jpg`;
- `POST /api/photo/capture`.

Use bounded request-body parsing, explicit response status, and chunked JPEG
transmission where required by `httpd_req_t`. Acquire a blob reference before
the first response chunk and release it on every completion and error path.

The capture POST must enqueue work and return 202. It must not call the camera
transaction synchronously from the HTTP handler.

### Exit gate

Test empty, ready, capturing, error, camera-unavailable, busy, invalid request,
304, 404, 410, 503, client disconnect, and concurrent replacement behavior.

## 9. Phase 7: RTSP Measurement and 640x480 Target

First run on-demand capture with no keepalive changes. Test connected VLC,
ffmpeg, and GStreamer clients. Record media gap, control-session behavior,
resume behavior, and reconnect behavior.

Only after a client demonstrably requires it may a keepalive change be added.

Then validate the 640x480 release stream:

1. Apply a complete direct 640x480 sensor descriptor if available.
2. Otherwise test 800x640 plus a proven hardware/ISP crop or resize path.
3. Verify actual V4L2 readback and decoded RTP dimensions.
4. Confirm the packetizer rejects non-stream still dimensions.

Do not call 800x640 or software-cropped 800x800 “VGA” without recording the
actual output dimensions.

### Exit gate

On-demand captures restore the stream for all tested clients without reboot.
The 640x480 target is validated, or a hardware limitation is documented as a
release blocker rather than silently accepted.

## 10. Phase 8: Background Scheduling and Settings

Add a one-entry capture request queue and a scheduler that measures the interval
from the completion of one attempt to the start of the next. Use a default
minimum interval of 1000 ms, but report actual cadence as:

```text
actual start-to-start cadence = transaction duration + configured interval
```

Missed ticks are not queued. Explicit requests have priority; overlapping
requests return 409.

Add `Preferences` settings only after the queue and controller are stable:

- `capture_mode`;
- `interval_ms`;
- `resolution`;
- `rtsp_policy`.

### Exit gate

Run 100 background attempts. Confirm no overlap, no queued backlog, no memory
trend, settings survive reboot, and measured cadence is reported accurately.

## 11. Optional Phases

### YUV420 optimization

Probe and test V4L2 output, JPEG input support, dimensions, colors, and memory
independently. Keep RGB565 as the fallback required path. Skip the phase if any
part is unavailable.

### 5 MP

Only attempt after the 1080p path is stable. Add a complete sensor table, run the
runtime memory gate, validate JPEG output, and retain 1080p as the maximum when
the board cannot meet the measured reserve.

### Web UI

A minimal settings page may be added after the API works. Gallery UI, runtime
quality controls, and diagnostic reset routes are not part of the core release.

## 12. Verification and Reporting

Every phase result document must include:

- status: complete, incomplete, or blocked;
- exact build command and toolchain version;
- actual serial log excerpts;
- measured memory, timings, dimensions, and rates;
- tests executed and pass/fail results;
- failures injected and resulting controller state;
- decisions and rationale;
- explicit readiness for the next phase.

Run these static checks after every source change:

```bash
git diff --check
rg -n "AsyncWebServer|ModeManager|capture_watchdog|/capture/1080p|/capture/5mp" \
  firmware mipi_csi_camera
```

The search must return no obsolete architecture references in the authoritative
implementation. A watchdog symbol is allowed only for whole-system reset
documentation, never as the capture timeout mechanism.

## 13. Final Release Gate

Release requires:

- reproducible ESP-IDF/Arduino-component build;
- Phase 0 baseline evidence;
- real bounded dequeue timeout;
- verified JPEG allocator release;
- one-owner CaptureController;
- 1080p RGB565 stills;
- mutex-safe PhotoStore;
- retained-photo API with metadata polling and ETags;
- no publication after restoration failure;
- measured RTSP behavior;
- validated 640x480 stream target or a documented blocking hardware result;
- 100 background attempts without overlap or memory trend;
- README and phase documents updated with measurements.

YUV420, 5 MP, and gallery UI do not block the core release.
