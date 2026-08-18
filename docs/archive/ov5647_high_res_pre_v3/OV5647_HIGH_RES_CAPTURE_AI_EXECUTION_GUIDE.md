# OV5647 High-Resolution Still Capture - AI Execution Guide

**Version:** 2.2  
**Last Updated:** 2026-08-18  
**Purpose:** Step-by-step implementation guide for AI assistants

**⚠️ IMPORTANT NOTICE:** This guide aligns with Plan V2.2 final architecture review.

**Key changes in v2.2:**
- **Timeout implementation:** ESP-Video VFS patching required (Tier 1), watchdog is degraded fallback only
- **Format priority:** RGB565 first and always, YUV420 is optional optimization after validation
- **Unified architecture:** CaptureController replaces ModeManager split pattern
- **PhotoStore safety:** Mutex-protected acquire() prevents use-after-free race
- **HTTP server:** esp_http_server (ESP-IDF native) required, not AsyncWebServer
- **API design:** Retained-photo API with metadata polling and conditional requests
- **Publication policy:** Do NOT publish if restoration fails
- **Photo ID semantics:** Increments only on successful publication (restoration must succeed)
- **JPEG allocator:** Verification and adapter pattern required before cleanup
- **RTSP keepalive:** Measurement-based approach (test first, add only if needed)
- **Memory gates:** Runtime measurement required, not estimates
- **Background cadence:** interval + transaction duration (not interval matching)

**⚠️ CODE SAFETY:** Code examples in this guide must be validated against the installed 
toolchain, ESP-Video version, and esp_cam_sensor structure before use. Do not execute 
code snippets literally - they are illustrative scaffolding that requires adaptation.

## Overview

This document provides detailed implementation instructions for the OV5647 high-resolution
still capture feature. It is designed for AI assistants executing the work described in
`OV5647_HIGH_RES_STILL_CAPTURE_PLAN_V2.md`.

### Quick Reference

- **Main Plan:** `docs/OV5647_HIGH_RES_STILL_CAPTURE_PLAN_V2.md` (v2.2)
- **Current Code:** `mipi_csi_camera/mipi_csi_camera.ino`
- **JPEG Encoder:** `mipi_csi_camera/jpeg_encoder.h/cpp`
- **Target Hardware:** ESP32-P4 with OV5647 MIPI-CSI camera

### Prerequisites

Before starting implementation:
1. Read and understand the main plan document v2.2
2. Review the current codebase structure
3. Understand the single-pipeline constraint
4. Verify Phase 0 hardware validation is complete
5. **Validate all code against actual installed APIs before use**

---

## Phase 0: Hardware Baseline Validation

**Goal:** Validate the 800x800 baseline on physical hardware.

**Status Check:** If Phase 0 is already complete, skip to Phase 1.

### Steps

#### 0.1 Flash Current Firmware to Hardware

```bash
# In Arduino IDE or platformio
# Upload mipi_csi_camera/mipi_csi_camera.ino to ESP32-P4 board
```

**What to verify:**
- Upload completes without errors
- Serial monitor shows initialization messages
- Wi-Fi connects successfully

#### 0.2 Capture Serial Log Output

Connect to serial monitor at 115200 baud and capture the following:

**Required log entries:**
```
memory milestone=boot heap_free=... psram_total=...
sensor identity status=confirmed pid=0x5647
vga probe status=... output=...
capture timeout status=unsupported dqbuf_wait=portMAX_DELAY
sensor baseline mode=... output=...x... fps=...
video baseline width=... height=... fourcc=...
```

**Action:** Save complete serial output to `docs/phase0_hardware_baseline.log`

#### 0.3 Test RTSP Stream

```bash
python3 tools/rtsp_viewer.py rtsp://<board-ip>:554/
```

**What to verify:**
- Stream connects successfully
- Video displays at approximately 10 FPS
- Resolution is 800x800 (or check serial log for actual dimensions)
- No excessive frame drops

**Action:** Save 5-second baseline metrics from serial log

#### 0.4 Document Hardware Results

Create `docs/phase0_results.md` with:
```markdown
# Phase 0 Hardware Validation Results

## Sensor Identity
- PID: 0x5647
- Status: [confirmed/failed]

## Baseline Stream
- Resolution: [actual]x[actual]
- Format: RGB565
- FPS: [measured from baseline window]
- JPEG Quality: 50
- Average JPEG size: [from baseline window]

## VGA Probe Result
- 640x480 support: [supported/unsupported]
- Next action: [validate-hardware-output / add-sensor-mode / use-800x800-baseline]

## Memory Baseline
- PSRAM Total: [value] MiB
- PSRAM Free after init: [value] MiB
- Largest free block: [value] MiB

## Timeout Mechanism
- DQBUF wait: portMAX_DELAY (blocking)
- VFS select hooks: no
- Next action: Implement Phase 1.5 timeout mechanism
```

**Exit Condition:** `phase0_results.md` is complete and shows successful baseline operation.

---

## Phase 1: Format Support Validation

**Goal:** Validate RGB565 high-res support, add 1080p sensor mode, optionally probe YUV420.

**Estimated Duration:** 4-6 hours

**CRITICAL:** RGB565 is the primary format. YUV420 is optional optimization only.

### Critical Decisions

This phase determines:
1. Whether RGB565 high-res works (required baseline)
2. Whether 1080p mode registers are correct
3. Whether YUV420 is available (optional memory optimization)
4. Whether 640x480 VGA is achievable (affects baseline)

### Steps

#### 1.1 Validate RGB565 High-Resolution Support

**Goal:** Confirm RGB565 works at 1080p resolution (required before proceeding).

**Add diagnostic function:**

```cpp
bool validateRgb565HighRes() {
  int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    Serial.println("rgb565 validation: failed to open device");
    return false;
  }

  // Test RGB565 format availability
  struct v4l2_fmtdesc fmtdesc = {};
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bool rgb565_found = false;

  for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0; fmtdesc.index++) {
    Serial.printf("format probe: index=%u fourcc=" V4L2_FMT_STR " description=%s\n",
                  fmtdesc.index, V4L2_FMT_STR_ARG(fmtdesc.pixelformat),
                  fmtdesc.description);
    
    if (fmtdesc.pixelformat == V4L2_PIX_FMT_RGB565) {
      rgb565_found = true;
    }
  }

  close(fd);
  Serial.printf("format validation: RGB565 support=%s\n", rgb565_found ? "YES" : "NO");
  return rgb565_found;
}
```

**Add to setup():**
```cpp
if (!validateRgb565HighRes()) {
  Serial.println("FATAL: RGB565 high-res not supported - cannot proceed");
  while(1) { delay(1000); }
}
```

**Exit condition:** RGB565 must be available. If not, this is a blocker - do not proceed.

#### 1.2 Probe YUV420 Format Support (Optional)

**Goal:** Check if YUV420 is available for future memory optimization.

**Add to validation function:**

```cpp
bool probeYuv420Support() {
  int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    Serial.println("yuv420 probe: failed to open device");
    return false;
  }

  struct v4l2_fmtdesc fmtdesc = {};
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bool yuv420_found = false;

  for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0; fmtdesc.index++) {
    if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUV420) {
      yuv420_found = true;
      break;
    }
  }

  close(fd);
  Serial.printf("yuv420 probe: support=%s (optional optimization)\n", 
                yuv420_found ? "AVAILABLE" : "NOT_AVAILABLE");
  return yuv420_found;
}
```

**Note:** YUV420 availability only affects Phase 3 optimization. Lack of YUV420 is NOT a blocker.

#### 1.3 Add 1080p Sensor Mode Descriptor

**Create:** `mipi_csi_camera/ov5647_modes.h`

```cpp
#pragma once

#include <esp_cam_sensor.h>

// Based on Espressif upstream OV5647 driver
// https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/sensors/ov5647/ov5647.c

static const esp_cam_sensor_format_t OV5647_1080P_RAW10 = {
  .name = "OV5647_1080P_30FPS_RAW10",
  .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
  .port = ESP_CAM_SENSOR_MIPI_CSI,
  .xclk = 24000000,
  .width = 1920,
  .height = 1080,
  .regs = nullptr,  // Will populate with register array
  .regs_size = 0,
  .fps = 30,
  .isp_2_byte_align = 0,
  .mipi_info = {
    .mipi_clk = 1000000000 / 2,  // 500 MHz
    .lane_num = 2,
    .line_sync_en = false,
  },
};

// Register configuration array - to be populated from upstream driver
// See: esp_cam_sensor/sensors/ov5647/ov5647.c in esp-video-components
```

**Action Items:**
1. Extract exact register array from Espressif upstream driver
2. Populate `OV5647_1080P_RAW10.regs` array
3. Set `regs_size` appropriately
4. Add include to main sketch

#### 1.4 Test 1080p Mode Application

**Add diagnostic function:**

```cpp
bool test1080pMode() {
  Serial.println("1080p test: stopping capture");
  capture_dev.stopCapture();
  
  int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    Serial.println("1080p test: failed to open device");
    return false;
  }

  Serial.println("1080p test: applying sensor format");
  if (ioctl(fd, VIDIOC_S_SENSOR_FMT, &OV5647_1080P_RAW10) != 0) {
    Serial.printf("1080p test: VIDIOC_S_SENSOR_FMT failed, errno=%d\n", errno);
    close(fd);
    return false;
  }

  // Readback to verify
  esp_cam_sensor_format_t readback = {};
  if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &readback) == 0) {
    Serial.printf("1080p test: readback width=%u height=%u format=%d fps=%u\n",
                  readback.width, readback.height, readback.format, readback.fps);
  }

  close(fd);
  
  // Restore baseline mode
  // TODO: Save original mode in setup() and restore here
  
  return true;
}
```

**Test in EXCLUDE_WIFI mode:**
```cpp
#ifdef EXCLUDE_WIFI
  // In setup(), after camera init
  test1080pMode();
#endif
```

#### 1.5 Add VGA 640x480 Mode (if needed)

**Check Phase 0 results:** If VGA probe showed "unsupported", create VGA mode descriptor.

**Option A: Direct 640x480 mode** (if register table is available)
```cpp
static const esp_cam_sensor_format_t OV5647_VGA_RAW8 = {
  .name = "OV5647_VGA_10FPS_RAW8",
  .format = ESP_CAM_SENSOR_PIXFORMAT_RAW8,
  .width = 640,
  .height = 480,
  // ... populate from OV5647 datasheet
};
```

**Option B: Use 800x640 baseline**
If 640x480 is unavailable, accept 800x640 as baseline (crop to 640x480 in software if needed).

**Decision Point:** Document chosen approach in Phase 1 results.

#### 1.6 Document Phase 1 Results

Create `docs/phase1_results.md`:

```markdown
# Phase 1 Format Validation Results

## RGB565 High-Res Support (REQUIRED)
- Status: [AVAILABLE / NOT AVAILABLE]
- If NOT AVAILABLE: BLOCKER - cannot proceed

## YUV420 Support (OPTIONAL)
- Status: [AVAILABLE / NOT AVAILABLE]
- Enumerated formats: [list from serial log]
- Plan adjustment: [none - will optimize in Phase 3 / not available - RGB565 only]

## 1080p Sensor Mode
- Application status: [SUCCESS / FAILED]
- Readback verification: [width x height]
- Ready for Phase 1.5: [YES / NO]

## VGA 640x480 Mode
- Approach: [direct mode / 800x640 baseline / 800x800 baseline]
- Rationale: [explanation]

## Updated Memory Budget
- High-res capture format: RGB565 (YUV420 available: [YES/NO])
- 1080p RGB565 peak PSRAM: 8.6 MiB
- 5MP RGB565 peak PSRAM: 17.9 MiB
- Available PSRAM: [from phase0] MiB
- Max resolution supported: [1080p / 5MP / requires validation]
```

**Exit Condition:** Phase 1 results documented, RGB565 confirmed working, 1080p mode applies successfully.

---

## Phase 1.5: Timeout Mechanism Gate (CRITICAL BLOCKER)

**Goal:** Implement bounded capture timeout mechanism via ESP-Video VFS patching.

**WARNING:** This phase BLOCKS all subsequent work. Do not proceed to Phase 2 until timeout mechanism is working.

**CRITICAL:** ESP-Video VFS patching is Tier 1 (required). Watchdog is Tier 2 (degraded fallback only if patching fails).

### Approach Decision

**Tier 1: Vendor ESP-Video with Timed Dequeue (REQUIRED)**
- Pros: Clean integration, proper timeout handling, bounded dequeue
- Cons: Requires maintaining vendored code
- Effort: 3-4 hours
- **Status: MANDATORY - attempt this first**

**Tier 2: Watchdog Task (DEGRADED FALLBACK ONLY)**
- Pros: No ESP-Video modification needed
- Cons: Cannot interrupt blocking dequeue, adds complexity, less reliable
- Effort: 4-5 hours
- **Status: Use ONLY if Tier 1 fails**
- **WARNING:** Watchdog cannot interrupt `xQueueReceive` - the capture thread will remain blocked even if timeout is detected. Recovery requires task termination.

### Implementation: Tier 1 (Vendored ESP-Video) - REQUIRED APPROACH

#### 1.5A.1 Locate ESP-Video Source

```bash
# Find ESP-Video library location
# Typically in:
# ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11/libraries/ESP_Video/
# Or in project libraries folder

# Copy to project
mkdir -p mipi_csi_camera/lib/ESP_Video_Patched
cp -r [ESP_Video_path]/* mipi_csi_camera/lib/ESP_Video_Patched/
```

#### 1.5A.2 Patch esp_video_vfs.c

Find the `VIDIOC_DQBUF` handling in `esp_video_vfs.c`:

```c
// Original code (blocking):
xQueueReceive(video_dev->filled_queue, &buffer, portMAX_DELAY);

// Patch to:
#define ESP_VIDEO_DQBUF_TIMEOUT_MS 5000

if (xQueueReceive(video_dev->filled_queue, &buffer, 
                  pdMS_TO_TICKS(ESP_VIDEO_DQBUF_TIMEOUT_MS)) != pdTRUE) {
    errno = ETIMEDOUT;
    return -1;
}
```

#### 1.5A.3 Update Include Path

In `mipi_csi_camera.ino`:
```cpp
// Replace: #include <ESP_Video.h>
#include "lib/ESP_Video_Patched/ESP_Video.h"
```

Document in `mipi_csi_camera/lib/ESP_Video_Patched/PATCHES.md`:
```markdown
# ESP-Video Patches

## Timed DQBUF
- File: src/esp_video_vfs.c
- Change: Added 5-second timeout to xQueueReceive in VIDIOC_DQBUF handler
- Reason: Enable capture timeout detection and recovery
- Upstream: Not submitted (project-specific requirement)
```

### Implementation: Tier 2 (Watchdog Task) - DEGRADED FALLBACK ONLY

**WARNING:** Only use this approach if Tier 1 (ESP-Video patching) is impossible.

**CRITICAL LIMITATION:** Watchdog cannot interrupt blocking `xQueueReceive` calls. The capture thread will remain blocked even after timeout detection. This requires:
- Task termination and recreation for recovery
- More complex state management
- Higher risk of incomplete cleanup

#### 1.5B.1 Create Watchdog Task

**Create:** `mipi_csi_camera/capture_watchdog.h`

```cpp
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>

class CaptureWatchdog {
public:
  void start();
  void stop();
  void beginCapture(uint32_t timeout_ms);
  void endCapture();
  bool hasTimedOut() const { return timed_out_; }
  void reset();

private:
  static void watchdogTask(void* param);
  
  TaskHandle_t task_handle_ = nullptr;
  std::atomic<bool> active_{false};
  std::atomic<bool> timed_out_{false};
  std::atomic<uint32_t> start_time_{0};
  std::atomic<uint32_t> timeout_ms_{0};
};
```

#### 1.5B.2 Implement Watchdog

**Create:** `mipi_csi_camera/capture_watchdog.cpp`

```cpp
#include "capture_watchdog.h"

void CaptureWatchdog::start() {
  xTaskCreate(watchdogTask, "capture_wd", 2048, this, 5, &task_handle_);
}

void CaptureWatchdog::stop() {
  if (task_handle_) {
    vTaskDelete(task_handle_);
    task_handle_ = nullptr;
  }
}

void CaptureWatchdog::beginCapture(uint32_t timeout_ms) {
  timed_out_ = false;
  timeout_ms_ = timeout_ms;
  start_time_ = millis();
  active_ = true;
}

void CaptureWatchdog::endCapture() {
  active_ = false;
}

void CaptureWatchdog::reset() {
  timed_out_ = false;
  active_ = false;
}

void CaptureWatchdog::watchdogTask(void* param) {
  CaptureWatchdog* self = static_cast<CaptureWatchdog*>(param);
  
  while (true) {
    if (self->active_) {
      uint32_t elapsed = millis() - self->start_time_;
      if (elapsed > self->timeout_ms_) {
        self->timed_out_ = true;
        self->active_ = false;
        Serial.println("WATCHDOG: capture timeout detected");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
  }
}
```

#### 1.5B.3 Integrate Watchdog

In main sketch:
```cpp
CaptureWatchdog watchdog;

void setup() {
  // After camera init
  watchdog.start();
}

// In capture code:
watchdog.beginCapture(5000);  // 5 second timeout
ESPVideoBufferClass frame = capture_dev.captureBuffer();
watchdog.endCapture();

if (watchdog.hasTimedOut()) {
  Serial.println("Capture timed out, initiating recovery");
  // Recovery logic
  watchdog.reset();
}
```

### 1.5.3 Test Timeout Mechanism

Create test that simulates hang:

```cpp
bool testTimeoutRecovery() {
  Serial.println("timeout test: starting");
  
  // Method 1: Stop providing frames (requires camera control)
  // Method 2: Capture with sensor in error state
  // Method 3: Manual validation with logic analyzer
  
  watchdog.beginCapture(2000);  // 2 second test timeout
  
  // Attempt capture (should timeout)
  uint32_t start = millis();
  ESPVideoBufferClass frame = capture_dev.captureBuffer();
  uint32_t elapsed = millis() - start;
  
  watchdog.endCapture();
  
  bool timeout_detected = watchdog.hasTimedOut();
  Serial.printf("timeout test: elapsed=%lums detected=%s\n",
                elapsed, timeout_detected ? "YES" : "NO");
  
  return timeout_detected;
}
```

**Manual validation required:** Since it's difficult to simulate a real sensor hang,
document the timeout implementation and mark for validation during Phase 2 actual mode switching.

#### 1.5.4 Document Timeout Approach

Create `docs/phase1_5_timeout_results.md`:

```markdown
# Phase 1.5 Timeout Mechanism Results

## Chosen Approach
- Tier 1: Vendored ESP-Video with VFS patching [ATTEMPTED / SUCCESS / FAILED]
- Tier 2: Watchdog Task (if Tier 1 failed) [ATTEMPTED / SUCCESS / NOT_NEEDED]

## Implementation Details
- Timeout duration: 5000ms
- Recovery mechanism: [bounded dequeue return / task termination]
- Tested scenarios: [list]

## Tier 1 Assessment (if attempted)
- ESP-Video source located: [YES / NO]
- VFS patch applied: [YES / NO]
- Compilation: [SUCCESS / FAILED]
- Timeout detection: [WORKING / NOT_WORKING]

## Tier 2 Assessment (if attempted)
- Watchdog task created: [YES / NO]
- Timeout detection: [WORKING / NOT_WORKING]
- Recovery complexity: [ACCEPTABLE / PROBLEMATIC]
- **Limitation documented:** Watchdog cannot interrupt blocking calls

## Validation
- Simulated hang test: [PASS / FAIL / MANUAL VALIDATION REQUIRED]
- Ready for Phase 2: [YES / NO]

## Files Modified/Created
- [list files]

## Critical Notes
- ESP-Video patching is strongly preferred over watchdog approach
- If using watchdog, document the blocking limitation in system docs
```

**Exit Condition:** Timeout mechanism is implemented with Tier 1 (patched VFS) or documented justification for Tier 2 (watchdog). Must demonstrate timeout detection capability.

---

## Phase 2: 1080p Still Capture with Retained-Photo API

**Goal:** Capture single 1080p frames using CaptureController state machine + retained-photo API.

**Prerequisites:** 
- Phase 1 complete (1080p mode validated, RGB565 confirmed)
- Phase 1.5 complete (timeout mechanism working)

**CRITICAL ARCHITECTURE CHANGES:**
- Use unified **CaptureController** (not ModeManager + StillCaptureHandler split)
- Use **esp_http_server** (ESP-IDF native, not AsyncWebServer)
- Implement **retained-photo API** (not synchronous capture endpoints)
- Use **PhotoStore** with mutex-protected acquire()
- Implement **do-NOT-publish-if-restoration-fails** policy

### Architecture Overview

```
CaptureController State Machine:
  Uninitialized → BaselineRunning → Stopping → SensorConfiguring 
  → BufferAllocating → HighResReady → Capturing → Restoring 
  → BaselineRunning (or Unavailable on failure)

HTTP GET /photo/latest (metadata only)
  → PhotoStore.acquire_latest()
  → Return: photo_id, width, height, size, timestamp
  
HTTP GET /photo/1234.jpg (with If-None-Match: "1234")
  → Check ETag match → 304 Not Modified
  → Otherwise: PhotoStore.acquire_by_id()
  → Stream JPEG, PhotoStore.release()

Background trigger (timer or manual):
  → CaptureController.capture_1080p()
    → State transitions with timeout protection
    → On success + restoration: PhotoStore.publish()
    → On failure or restoration failure: do NOT publish
```

### Steps

#### 2.1 Create PhotoStore Module with Mutex-Protected Acquire

**Create:** `mipi_csi_camera/photo_store.h`

```cpp
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

struct PhotoBlob {
  uint32_t photo_id;
  uint8_t* jpeg_data;
  size_t jpeg_size;
  uint32_t width;
  uint32_t height;
  uint64_t timestamp_us;
  std::atomic<uint32_t> ref_count;
  
  PhotoBlob() : photo_id(0), jpeg_data(nullptr), jpeg_size(0), 
                width(0), height(0), timestamp_us(0), ref_count(0) {}
};

class PhotoStore {
public:
  PhotoStore();
  ~PhotoStore();
  
  bool init(uint64_t boot_generation);
  
  // Publish new photo (increments photo_id)
  // CRITICAL: Only call if restoration succeeded
  void publish(uint8_t* jpeg_data, size_t jpeg_size, 
               uint32_t width, uint32_t height);
  
  // Acquire latest photo (mutex-protected, increments ref_count)
  PhotoBlob* acquire_latest();
  
  // Acquire specific photo by ID
  PhotoBlob* acquire_by_id(uint32_t photo_id);
  
  // Release photo (decrements ref_count, frees if zero)
  void release(PhotoBlob* photo);
  
  // Get latest photo_id without acquiring
  uint32_t get_latest_id() const { return next_photo_id_ - 1; }

private:
  SemaphoreHandle_t mutex_;
  PhotoBlob* current_;
  uint32_t next_photo_id_;
  uint64_t boot_generation_;
};
```

**Create:** `mipi_csi_camera/photo_store.cpp`

```cpp
#include "photo_store.h"

PhotoStore::PhotoStore() 
  : mutex_(nullptr), current_(nullptr), next_photo_id_(1), boot_generation_(0) {
}

PhotoStore::~PhotoStore() {
  if (mutex_) {
    vSemaphoreDelete(mutex_);
  }
  if (current_) {
    free(current_->jpeg_data);
    delete current_;
  }
}

bool PhotoStore::init(uint64_t boot_generation) {
  boot_generation_ = boot_generation;
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) {
    Serial.println("photo_store: failed to create mutex");
    return false;
  }
  Serial.printf("photo_store: initialized boot_gen=%llu\n", boot_generation_);
  return true;
}

void PhotoStore::publish(uint8_t* jpeg_data, size_t jpeg_size, 
                         uint32_t width, uint32_t height) {
  PhotoBlob* new_photo = new PhotoBlob();
  new_photo->photo_id = next_photo_id_++;
  new_photo->jpeg_data = jpeg_data;
  new_photo->jpeg_size = jpeg_size;
  new_photo->width = width;
  new_photo->height = height;
  new_photo->timestamp_us = esp_timer_get_time();
  new_photo->ref_count.store(0, std::memory_order_relaxed);
  
  xSemaphoreTake(mutex_, portMAX_DELAY);
  
  PhotoBlob* old_photo = current_;
  current_ = new_photo;
  
  xSemaphoreGive(mutex_);
  
  // Clean up old photo if no references
  if (old_photo && old_photo->ref_count.load(std::memory_order_acquire) == 0) {
    free(old_photo->jpeg_data);
    delete old_photo;
  }
  
  Serial.printf("photo_store: published photo_id=%u size=%zu\n", 
                new_photo->photo_id, jpeg_size);
}

PhotoBlob* PhotoStore::acquire_latest() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  
  PhotoBlob* result = current_;
  if (result) {
    result->ref_count.fetch_add(1, std::memory_order_acquire);
  }
  
  xSemaphoreGive(mutex_);
  return result;
}

PhotoBlob* PhotoStore::acquire_by_id(uint32_t photo_id) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  
  PhotoBlob* result = nullptr;
  if (current_ && current_->photo_id == photo_id) {
    result = current_;
    result->ref_count.fetch_add(1, std::memory_order_acquire);
  }
  
  xSemaphoreGive(mutex_);
  return result;
}

void PhotoStore::release(PhotoBlob* photo) {
  if (!photo) return;
  
  uint32_t prev_count = photo->ref_count.fetch_sub(1, std::memory_order_release);
  
  // If this was the last reference and it's not current, free it
  if (prev_count == 1) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool is_current = (current_ == photo);
    xSemaphoreGive(mutex_);
    
    if (!is_current) {
      free(photo->jpeg_data);
      delete photo;
    }
  }
}
```

#### 2.2 Create CaptureController with State Machine

**Create:** `mipi_csi_camera/capture_controller.h`

```cpp
#pragma once

#include <Arduino.h>
#include <ESP_Video.h>
#include <esp_cam_sensor.h>
#include "photo_store.h"

enum class ControllerState {
  Uninitialized,
  BaselineRunning,
  Stopping,
  SensorConfiguring,
  BufferAllocating,
  HighResReady,
  Capturing,
  Restoring,
  Unavailable
};

struct CaptureResult {
  bool success;
  bool restoration_succeeded;
  String error_message;
  uint32_t capture_time_ms;
};

class CaptureController {
public:
  bool init(ESPVideoClass* capture_dev, PhotoStore* photo_store,
            int video_fd, uint64_t boot_generation);
  
  // Capture with full transaction (includes restoration)
  CaptureResult capture_1080p(uint32_t timeout_ms = 5000);
  
  ControllerState get_state() const { return state_; }
  
private:
  bool transition_to(ControllerState new_state);
  bool stop_baseline();
  bool configure_sensor_1080p();
  bool allocate_buffers_1080p();
  bool capture_frame(uint32_t timeout_ms, ESPVideoBufferClass& frame);
  bool restore_baseline();
  
  ESPVideoClass* capture_dev_;
  PhotoStore* photo_store_;
  int video_fd_;
  uint64_t boot_generation_;
  ControllerState state_;
  
  esp_cam_sensor_format_t baseline_sensor_fmt_;
  esp_cam_sensor_format_t mode_1080p_;
};
```

**Create:** `mipi_csi_camera/capture_controller.cpp`

```cpp
#include "capture_controller.h"
#include "jpeg_encoder.h"
#include <fcntl.h>
#include <sys/ioctl.h>

extern const esp_cam_sensor_format_t OV5647_1080P_RAW10;

bool CaptureController::init(ESPVideoClass* capture_dev, PhotoStore* photo_store,
                              int video_fd, uint64_t boot_generation) {
  capture_dev_ = capture_dev;
  photo_store_ = photo_store;
  video_fd_ = video_fd;
  boot_generation_ = boot_generation;
  state_ = ControllerState::Uninitialized;
  
  // Save baseline sensor format
  if (ioctl(video_fd_, VIDIOC_G_SENSOR_FMT, &baseline_sensor_fmt_) != 0) {
    Serial.println("capture_ctrl: failed to read baseline sensor format");
    return false;
  }
  
  // Load 1080p mode
  mode_1080p_ = OV5647_1080P_RAW10;
  
  transition_to(ControllerState::BaselineRunning);
  Serial.printf("capture_ctrl: initialized, baseline=%ux%u\n",
                baseline_sensor_fmt_.width, baseline_sensor_fmt_.height);
  return true;
}

CaptureResult CaptureController::capture_1080p(uint32_t timeout_ms) {
  CaptureResult result = {};
  result.success = false;
  result.restoration_succeeded = false;
  
  Serial.println("capture_ctrl: starting 1080p capture transaction");
  uint32_t txn_start = millis();
  
  // State: BaselineRunning → Stopping
  if (!stop_baseline()) {
    result.error_message = "failed to stop baseline";
    transition_to(ControllerState::Unavailable);
    return result;
  }
  
  // State: Stopping → SensorConfiguring
  if (!configure_sensor_1080p()) {
    result.error_message = "sensor configuration failed";
    restore_baseline(); // Attempt recovery
    return result;
  }
  
  // State: SensorConfiguring → BufferAllocating
  if (!allocate_buffers_1080p()) {
    result.error_message = "buffer allocation failed";
    restore_baseline();
    return result;
  }
  
  // State: BufferAllocating → HighResReady → Capturing
  transition_to(ControllerState::HighResReady);
  transition_to(ControllerState::Capturing);
  
  ESPVideoBufferClass frame;
  if (!capture_frame(timeout_ms, frame)) {
    result.error_message = "capture timeout or frame null";
    restore_baseline();
    return result;
  }
  
  result.capture_time_ms = millis() - txn_start;
  Serial.printf("capture_ctrl: frame captured in %ums, encoding JPEG\n", 
                result.capture_time_ms);
  
  // JPEG encode
  size_t jpeg_size = 0;
  uint8_t* jpeg_buf = encodeJpeg((uint8_t*)frame.getBufferPtr(),
                                  1920, 1080,
                                  PIXFORMAT_RGB565,
                                  80, &jpeg_size);
  
  if (!jpeg_buf) {
    result.error_message = "JPEG encoding failed";
    restore_baseline();
    return result;
  }
  
  // State: Capturing → Restoring
  transition_to(ControllerState::Restoring);
  result.restoration_succeeded = restore_baseline();
  
  if (result.restoration_succeeded) {
    // SUCCESS: Publish the photo
    photo_store_->publish(jpeg_buf, jpeg_size, 1920, 1080);
    result.success = true;
    Serial.printf("capture_ctrl: transaction complete, published photo\n");
  } else {
    // FAILURE: Do NOT publish, free buffer
    free(jpeg_buf);
    result.error_message = "restoration failed";
    Serial.println("capture_ctrl: restoration failed, photo NOT published");
  }
  
  return result;
}

bool CaptureController::transition_to(ControllerState new_state) {
  Serial.printf("capture_ctrl: state %d → %d\n", (int)state_, (int)new_state);
  state_ = new_state;
  return true;
}

bool CaptureController::stop_baseline() {
  transition_to(ControllerState::Stopping);
  capture_dev_->stopCapture();
  return true;
}

bool CaptureController::configure_sensor_1080p() {
  transition_to(ControllerState::SensorConfiguring);
  
  if (ioctl(video_fd_, VIDIOC_S_SENSOR_FMT, &mode_1080p_) != 0) {
    Serial.printf("capture_ctrl: sensor config failed, errno=%d\n", errno);
    return false;
  }
  
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = 1920;
  fmt.fmt.pix.height = 1080;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  
  if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    Serial.printf("capture_ctrl: video format failed, errno=%d\n", errno);
    return false;
  }
  
  return true;
}

bool CaptureController::allocate_buffers_1080p() {
  transition_to(ControllerState::BufferAllocating);
  
  if (!capture_dev_->startCapture()) {
    Serial.println("capture_ctrl: failed to start capture");
    return false;
  }
  
  return true;
}

bool CaptureController::capture_frame(uint32_t timeout_ms, ESPVideoBufferClass& frame) {
  // TODO: Integrate timeout mechanism from Phase 1.5
  // For Tier 1 (patched VFS): timeout is automatic
  // For Tier 2 (watchdog): need external coordination
  
  uint32_t start = millis();
  frame = capture_dev_->captureBuffer();
  uint32_t elapsed = millis() - start;
  
  if (!frame) {
    Serial.printf("capture_ctrl: frame null after %ums\n", elapsed);
    return false;
  }
  
  if (elapsed > timeout_ms) {
    Serial.printf("capture_ctrl: timeout %ums > %ums\n", elapsed, timeout_ms);
    return false;
  }
  
  return true;
}

bool CaptureController::restore_baseline() {
  transition_to(ControllerState::Restoring);
  
  capture_dev_->stopCapture();
  delay(100);
  
  // Restore baseline sensor format
  if (ioctl(video_fd_, VIDIOC_S_SENSOR_FMT, &baseline_sensor_fmt_) != 0) {
    Serial.println("capture_ctrl: baseline sensor restore failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  // Restore baseline video format
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = baseline_sensor_fmt_.width;
  fmt.fmt.pix.height = baseline_sensor_fmt_.height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  
  if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    Serial.println("capture_ctrl: baseline video restore failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  if (!capture_dev_->startCapture()) {
    Serial.println("capture_ctrl: baseline restart failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  transition_to(ControllerState::BaselineRunning);
  Serial.println("capture_ctrl: baseline restored successfully");
  return true;
}
```

#### 2.3 Implement Retained-Photo API with esp_http_server

**Create:** `mipi_csi_camera/photo_api.h`

```cpp
#pragma once

#include <esp_http_server.h>
#include "photo_store.h"

class PhotoAPI {
public:
  bool init(httpd_handle_t server, PhotoStore* photo_store);
  
private:
  static esp_err_t handle_photo_latest(httpd_req_t* req);
  static esp_err_t handle_photo_by_id(httpd_req_t* req);
  
  static PhotoStore* photo_store_;
};
```

**Create:** `mipi_csi_camera/photo_api.cpp`

```cpp
#include "photo_api.h"
#include <Arduino.h>

PhotoStore* PhotoAPI::photo_store_ = nullptr;

bool PhotoAPI::init(httpd_handle_t server, PhotoStore* photo_store) {
  photo_store_ = photo_store;
  
  // Register /photo/latest endpoint (metadata only)
  httpd_uri_t latest_uri = {
    .uri = "/photo/latest",
    .method = HTTP_GET,
    .handler = handle_photo_latest,
    .user_ctx = nullptr
  };
  
  if (httpd_register_uri_handler(server, &latest_uri) != ESP_OK) {
    Serial.println("photo_api: failed to register /photo/latest");
    return false;
  }
  
  // Register /photo/*.jpg endpoint (JPEG data with conditional request support)
  httpd_uri_t photo_uri = {
    .uri = "/photo/*.jpg",
    .method = HTTP_GET,
    .handler = handle_photo_by_id,
    .user_ctx = nullptr
  };
  
  if (httpd_register_uri_handler(server, &photo_uri) != ESP_OK) {
    Serial.println("photo_api: failed to register /photo/*.jpg");
    return false;
  }
  
  Serial.println("photo_api: endpoints registered");
  return true;
}

esp_err_t PhotoAPI::handle_photo_latest(httpd_req_t* req) {
  PhotoBlob* photo = photo_store_->acquire_latest();
  
  if (!photo) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No photo available");
    return ESP_OK;
  }
  
  // Return metadata as JSON
  char json[256];
  snprintf(json, sizeof(json),
           "{\"photo_id\":%u,\"width\":%u,\"height\":%u,"
           "\"size\":%zu,\"timestamp\":%llu}",
           photo->photo_id, photo->width, photo->height,
           photo->jpeg_size, photo->timestamp_us);
  
  photo_store_->release(photo);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, strlen(json));
  
  return ESP_OK;
}

esp_err_t PhotoAPI::handle_photo_by_id(httpd_req_t* req) {
  // Parse photo_id from URI: /photo/1234.jpg
  const char* uri = req->uri;
  uint32_t photo_id = 0;
  if (sscanf(uri, "/photo/%u.jpg", &photo_id) != 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid photo ID");
    return ESP_OK;
  }
  
  // Check If-None-Match header for conditional request
  char etag_header[64];
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", etag_header, 
                                   sizeof(etag_header)) == ESP_OK) {
    uint32_t client_id = 0;
    if (sscanf(etag_header, "\"%u\"", &client_id) == 1 && client_id == photo_id) {
      // Client has current version
      httpd_resp_set_status(req, "304 Not Modified");
      httpd_resp_send(req, nullptr, 0);
      return ESP_OK;
    }
  }
  
  // Acquire photo
  PhotoBlob* photo = photo_store_->acquire_by_id(photo_id);
  
  if (!photo) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
    return ESP_OK;
  }
  
  // Set headers
  httpd_resp_set_type(req, "image/jpeg");
  
  char etag[32];
  snprintf(etag, sizeof(etag), "\"%u\"", photo->photo_id);
  httpd_resp_set_hdr(req, "ETag", etag);
  
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
  
  char disposition[64];
  snprintf(disposition, sizeof(disposition), "inline; filename=%u.jpg", photo->photo_id);
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);
  
  // Stream JPEG data
  esp_err_t ret = httpd_resp_send(req, (const char*)photo->jpeg_data, photo->jpeg_size);
  
  photo_store_->release(photo);
  
  Serial.printf("photo_api: served photo_id=%u size=%zu\n", 
                photo->photo_id, photo->jpeg_size);
  
  return ret;
}
```

**Note:** This uses ESP-IDF native `esp_http_server`, not AsyncWebServer.

#### 2.4 Integration in Main Sketch

**In main sketch setup():**

```cpp
#include "photo_store.h"
#include "capture_controller.h"
#include "photo_api.h"
#include <esp_http_server.h>

// Global instances
PhotoStore photo_store;
CaptureController capture_controller;
PhotoAPI photo_api;
httpd_handle_t http_server = nullptr;

void setup() {
  Serial.begin(115200);
  
  // Generate boot generation
  uint64_t boot_generation = ((uint64_t)esp_random() << 32) | esp_random();
  Serial.printf("boot generation=%llu\n", boot_generation);
  
  // Initialize camera (existing code)
  // ...
  
  // Initialize PhotoStore
  if (!photo_store.init(boot_generation)) {
    Serial.println("FATAL: PhotoStore init failed");
    while(1) delay(1000);
  }
  
  // Get video file descriptor
  int video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (video_fd < 0) {
    Serial.println("FATAL: Cannot open video device");
    while(1) delay(1000);
  }
  
  // Initialize CaptureController
  if (!capture_controller.init(&capture_dev, &photo_store, video_fd, boot_generation)) {
    Serial.println("FATAL: CaptureController init failed");
    while(1) delay(1000);
  }
  
  // Start HTTP server (esp_http_server)
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  
  if (httpd_start(&http_server, &config) != ESP_OK) {
    Serial.println("FATAL: HTTP server start failed");
    while(1) delay(1000);
  }
  
  // Register photo API endpoints
  if (!photo_api.init(http_server, &photo_store)) {
    Serial.println("FATAL: PhotoAPI init failed");
    while(1) delay(1000);
  }
  
  Serial.println("System ready - retained-photo API active");
}

void loop() {
  // Background capture trigger (example: every 30 seconds)
  static uint32_t last_capture = 0;
  uint32_t now = millis();
  
  if (now - last_capture > 30000) {
    Serial.println("Background: triggering 1080p capture");
    CaptureResult result = capture_controller.capture_1080p();
    
    if (result.success) {
      Serial.printf("Background: capture succeeded, photo published\n");
    } else {
      Serial.printf("Background: capture failed - %s\n", result.error_message.c_str());
    }
    
    last_capture = now + result.capture_time_ms; // Add transaction time
  }
  
  delay(100);
}
```

**Key points:**
- Use `esp_http_server`, not AsyncWebServer
- PhotoStore initialized with boot_generation
- CaptureController manages entire transaction
- Background trigger uses `interval + transaction_time` cadence
- Photo only published if restoration succeeds

#### 2.5 Test Retained-Photo API

**Test procedure:**

1. **Flash and monitor:**
   ```bash
   # Upload firmware
   # Open serial monitor at 115200 baud
   ```

2. **Verify baseline stream:**
   ```bash
   python3 tools/rtsp_viewer.py rtsp://<board-ip>:554/
   # Should see 800x800 stream at ~10 FPS
   ```

3. **Wait for first background capture:**
   - Watch serial output for "Background: triggering 1080p capture"
   - Should see state transitions: BaselineRunning → Stopping → SensorConfiguring → BufferAllocating → HighResReady → Capturing → Restoring → BaselineRunning
   - Should see "photo_store: published photo_id=1"

4. **Query photo metadata:**
   ```bash
   curl http://<board-ip>/photo/latest
   # Expected: {"photo_id":1,"width":1920,"height":1080,"size":...,"timestamp":...}
   ```

5. **Fetch photo JPEG:**
   ```bash
   curl -o test_1080p.jpg http://<board-ip>/photo/1.jpg
   file test_1080p.jpg  # Should show: JPEG image data
   identify test_1080p.jpg  # Should show: 1920x1080
   ```

6. **Test conditional request (ETag):**
   ```bash
   # First request returns 200 with ETag
   curl -v http://<board-ip>/photo/1.jpg -o /dev/null
   # Note the ETag header: "1"
   
   # Second request with If-None-Match returns 304
   curl -v -H 'If-None-Match: "1"' http://<board-ip>/photo/1.jpg
   # Should see: 304 Not Modified
   ```

7. **Verify baseline restoration:**
   ```bash
   # Check RTSP viewer - stream should continue without interruption
   # Serial log should show: "capture_ctrl: baseline restored successfully"
   ```

8. **Test restoration failure scenario:**
   - Trigger capture during sensor disconnection (if possible)
   - Should see: "capture_ctrl: restoration failed, photo NOT published"
   - Query /photo/latest should return previous photo_id (not incremented)

**Expected serial output:**
```
Background: triggering 1080p capture
capture_ctrl: starting 1080p capture transaction
capture_ctrl: state 1 → 2  (BaselineRunning → Stopping)
capture_ctrl: state 2 → 3  (Stopping → SensorConfiguring)
capture_ctrl: state 3 → 4  (SensorConfiguring → BufferAllocating)
capture_ctrl: state 4 → 5  (BufferAllocating → HighResReady)
capture_ctrl: state 5 → 6  (HighResReady → Capturing)
capture_ctrl: frame captured in 287ms, encoding JPEG
capture_ctrl: state 6 → 7  (Capturing → Restoring)
capture_ctrl: baseline restored successfully
capture_ctrl: state 7 → 1  (Restoring → BaselineRunning)
capture_ctrl: transaction complete, published photo
photo_store: published photo_id=1 size=234567
Background: capture succeeded, photo published
```

#### 2.6 Measure Performance

Add diagnostic endpoint for batch testing:

```cpp
// Add to photo_api.cpp or main sketch
esp_err_t handle_test_batch(httpd_req_t* req) {
  const int NUM_CAPTURES = 10;
  uint32_t times[NUM_CAPTURES];
  int successes = 0;
  int restoration_failures = 0;
  
  for (int i = 0; i < NUM_CAPTURES; i++) {
    CaptureResult result = capture_controller.capture_1080p();
    if (result.success) {
      times[successes] = result.capture_time_ms;
      successes++;
    } else if (!result.restoration_succeeded) {
      restoration_failures++;
    }
    delay(100);
  }
  
  // Calculate statistics
  uint32_t total_time = 0, min_time = UINT32_MAX, max_time = 0;
  for (int i = 0; i < successes; i++) {
    total_time += times[i];
    if (times[i] < min_time) min_time = times[i];
    if (times[i] > max_time) max_time = times[i];
  }
  
  char response[512];
  snprintf(response, sizeof(response),
           "1080p Batch Test Results\n"
           "Successes: %d/%d\n"
           "Restoration failures: %d\n"
           "Avg time: %lums\n"
           "Min time: %lums\n"
           "Max time: %lums\n",
           successes, NUM_CAPTURES, restoration_failures,
           successes > 0 ? total_time / successes : 0,
           min_time, max_time);
  
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, response, strlen(response));
  
  return ESP_OK;
}
```

**Expected performance:**
- Capture time: 200-500ms (mode switch + frame + JPEG encode + restoration)
- JPEG size: 150-300 KB at quality 80
- Success rate: 100% (with timeout mechanism preventing hangs)
- Restoration success rate: 100% (critical for publication)

#### 2.7 Document Phase 2 Results

Create `docs/phase2_results.md`:

```markdown
# Phase 2: 1080p Still Capture with Retained-Photo API Results

## Implementation Status
- PhotoStore with mutex-protected acquire: [COMPLETE / INCOMPLETE]
- CaptureController state machine: [COMPLETE / INCOMPLETE]
- esp_http_server integration: [COMPLETE / INCOMPLETE]
- Retained-photo API endpoints: [COMPLETE / INCOMPLETE]
- Timeout integration: [WORKING / ISSUES]

## Architecture Validation
- Unified CaptureController (not split): [YES / NO]
- esp_http_server (not AsyncWebServer): [YES / NO]
- Retained-photo API (not synchronous): [YES / NO]
- Mutex-protected PhotoStore.acquire(): [YES / NO]
- Do-NOT-publish-if-restoration-fails: [IMPLEMENTED / NOT_IMPLEMENTED]

## Functional Testing
- Background capture trigger: [WORKING / ISSUES]
- /photo/latest metadata: [WORKING / ISSUES]
- /photo/N.jpg JPEG fetch: [WORKING / ISSUES]
- Conditional request (ETag): [WORKING / ISSUES]
- Baseline restoration: [AUTOMATIC / MANUAL / FAILS]
- Timeout recovery: [TESTED / UNTESTED]
- Restoration failure handling: [TESTED / UNTESTED]

## Performance Metrics (10 captures)
- Success rate: X/10
- Restoration success rate: X/10 (must be 10/10 for published photos)
- Average capture time: XXX ms
- Min/Max time: XXX / XXX ms
- Average JPEG size: XXX KB
- JPEG quality: 80

## State Machine Validation
- All transitions logged: [YES / NO]
- Unavailable state handling: [TESTED / UNTESTED]
- State recovery on failure: [WORKING / ISSUES]

## Photo ID Semantics
- Increments only on successful publication: [VERIFIED / NOT_VERIFIED]
- Does NOT increment on restoration failure: [VERIFIED / NOT_VERIFIED]

## Issues Encountered
- [List any issues and resolutions]

## Memory Observations
- PSRAM usage during 1080p RGB565: [observed] MiB
- PhotoStore memory management: [STABLE / LEAKS]
- Heap fragmentation: [none / minor / severe]

## Ready for Phase 3
- [YES / NO - explain if NO]
```

**Exit Condition:** 1080p captures work with retained-photo API, CaptureController state machine functions correctly, PhotoStore is thread-safe, restoration failures prevent publication, baseline stream restores automatically.

---

## Phase 3: Memory Optimization (YUV420) - OPTIONAL

**Goal:** Switch to YUV420 format to reduce memory footprint by 33% (4.0 MiB → 3.0 MiB for 1080p).

**Prerequisites:**
- Phase 1 confirmed YUV420 is available
- Phase 2 working with RGB565

**CRITICAL:** This phase is OPTIONAL. Skip if Phase 1 showed YUV420 unavailable.

**Skip if:** Phase 1 showed YUV420 is NOT available - document why and proceed to Phase 4 with RGB565.

### Steps

#### 3.1 Update CaptureController for YUV420

In `capture_controller.cpp`, modify `configure_sensor_1080p()`:

```cpp
bool CaptureController::configure_sensor_1080p() {
  transition_to(ControllerState::SensorConfiguring);
  
  if (ioctl(video_fd_, VIDIOC_S_SENSOR_FMT, &mode_1080p_) != 0) {
    Serial.printf("capture_ctrl: sensor config failed, errno=%d\n", errno);
    return false;
  }
  
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = 1920;
  fmt.fmt.pix.height = 1080;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;  // Changed from RGB565
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  
  if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    Serial.printf("capture_ctrl: video format failed, errno=%d\n", errno);
    return false;
  }
  
  return true;
}
```

#### 3.2 Update JPEG Encoder Call

In `capture_controller.cpp`, update the `capture_1080p()` method:

```cpp
// Change from:
uint8_t* jpeg_buf = encodeJpeg((uint8_t*)frame.getBufferPtr(),
                                1920, 1080, 
                                PIXFORMAT_RGB565,  // Old
                                80, &jpeg_size);

// To:
uint8_t* jpeg_buf = encodeJpeg((uint8_t*)frame.getBufferPtr(),
                                1920, 1080, 
                                PIXFORMAT_YUV420,  // New
                                80, &jpeg_size);
```

**Verify:** Check that `jpeg_encoder.cpp` supports `PIXFORMAT_YUV420` input format.
If not, add support or use esp_jpg library directly.

#### 3.3 Test YUV420 Capture

```bash
# Wait for background capture or trigger manually
# Check serial log for successful capture with YUV420

# Fetch photo
curl http://<board-ip>/photo/latest
# Note the photo_id

curl -o test_1080p_yuv420.jpg http://<board-ip>/photo/<id>.jpg
identify test_1080p_yuv420.jpg  # Verify 1920x1080 and valid JPEG
```

**Compare:**
- RGB565 JPEG size: [from Phase 2] KB
- YUV420 JPEG size: [measure] KB
- Expect: Similar or slightly smaller (YUV420 is more compressible)

#### 3.4 Measure Memory Savings

Add memory diagnostics in `capture_controller.cpp`:

```cpp
void logMemoryState(const char* label) {
  Serial.printf("memory %s heap_free=%u psram_free=%u largest_free=%u\n",
                label,
                ESP.getFreeHeap(),
                ESP.getFreePsram(),
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// In capture_1080p(), before and after frame capture:
bool CaptureController::capture_frame(uint32_t timeout_ms, ESPVideoBufferClass& frame) {
  logMemoryState("before_capture");
  
  uint32_t start = millis();
  frame = capture_dev_->captureBuffer();
  uint32_t elapsed = millis() - start;
  
  logMemoryState("after_capture");
  
  // ... rest of function
}
```

**Expected savings:**
- RGB565 1080p frame: 4.0 MiB
- YUV420 1080p frame: 3.0 MiB
- Savings: 1.0 MiB per frame (25% reduction)

#### 3.5 Update Baseline Restoration for YUV420

Ensure baseline also uses appropriate format. In `capture_controller.cpp`:

```cpp
bool CaptureController::restore_baseline() {
  transition_to(ControllerState::Restoring);
  
  capture_dev_->stopCapture();
  delay(100);
  
  // Restore baseline sensor format
  if (ioctl(video_fd_, VIDIOC_S_SENSOR_FMT, &baseline_sensor_fmt_) != 0) {
    Serial.println("capture_ctrl: baseline sensor restore failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  // Restore baseline video format (RGB565 for baseline streaming)
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = baseline_sensor_fmt_.width;
  fmt.fmt.pix.height = baseline_sensor_fmt_.height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;  // Baseline uses RGB565
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  
  if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    Serial.println("capture_ctrl: baseline video restore failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  if (!capture_dev_->startCapture()) {
    Serial.println("capture_ctrl: baseline restart failed");
    transition_to(ControllerState::Unavailable);
    return false;
  }
  
  transition_to(ControllerState::BaselineRunning);
  Serial.println("capture_ctrl: baseline restored successfully");
  return true;
}
```

#### 3.6 Document Phase 3 Results

Create `docs/phase3_results.md`:

```markdown
# Phase 3: YUV420 Optimization Results

## YUV420 Availability
- Status: [AVAILABLE / NOT AVAILABLE]
- Phase execution: [COMPLETED / SKIPPED]
- Reason if skipped: [Phase 1 showed unavailable / other]

## Memory Impact (if completed)
- RGB565 1080p frame size: 4.0 MiB
- YUV420 1080p frame size: 3.0 MiB
- Memory saved: 1.0 MiB (25% reduction)
- Measured PSRAM delta: [actual from logs] MiB

## JPEG Quality Comparison
- RGB565 JPEG size: XXX KB
- YUV420 JPEG size: XXX KB
- Visual quality: [EQUIVALENT / DEGRADED / IMPROVED]
- Compression efficiency: [BETTER / SAME / WORSE]

## Performance
- RGB565 capture time: XXX ms (from Phase 2)
- YUV420 capture time: XXX ms
- Encoding time change: [FASTER / SLOWER / SAME]

## Functional Validation
- YUV420 capture success: [YES / NO]
- Baseline restoration: [WORKING / ISSUES]
- JPEG encoding compatibility: [WORKING / ISSUES]

## Ready for Phase 4
- [YES / NO]
- Memory budget for 5MP: [sufficient / insufficient]
```

**Exit Condition:** YUV420 format validated and memory savings confirmed, OR documented as skipped with rationale (Phase 1 showed unavailable). RGB565 remains fully functional.

---

## Phase 4: 5MP Still Capture

**Goal:** Add 2592x1944 (5MP) capture capability using CaptureController.

**Prerequisites:**
- Phases 2-3 complete
- Memory headroom validated (Phase 1 calculations)

### Critical Pre-Check

**Before starting Phase 4, verify memory budget with runtime measurement:**

Add diagnostic to validate 5MP feasibility:

```cpp
// Add endpoint or run in EXCLUDE_WIFI mode
void checkMemoryFor5MP() {
  size_t psram_total = ESP.getPsramSize();
  size_t psram_free = ESP.getFreePsram();
  size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  
  // Calculate 5MP frame size
  size_t frame_5mp_rgb565 = 2592 * 1944 * 2;  // 9.76 MiB
  size_t frame_5mp_yuv420 = (2592 * 1944 * 3) / 2;  // 7.32 MiB
  
  // Use current format (RGB565 or YUV420 from Phase 3)
  bool using_yuv420 = /* check Phase 3 results */;
  size_t frame_needed = using_yuv420 ? frame_5mp_yuv420 : frame_5mp_rgb565;
  
  // Add 2MB JPEG buffer + 1MB overhead
  size_t total_needed = frame_needed + (2 * 1024 * 1024) + (1 * 1024 * 1024);
  
  Serial.printf("5MP Memory Budget Check\n");
  Serial.printf("PSRAM total: %zu MiB\n", psram_total / 1024 / 1024);
  Serial.printf("PSRAM free: %zu MiB\n", psram_free / 1024 / 1024);
  Serial.printf("Largest block: %zu MiB\n", largest_block / 1024 / 1024);
  Serial.printf("5MP frame (%s): %zu MiB\n", 
                using_yuv420 ? "YUV420" : "RGB565",
                frame_needed / 1024 / 1024);
  Serial.printf("Estimated total: %zu MiB\n", total_needed / 1024 / 1024);
  Serial.printf("Status: %s\n", 
                (psram_free >= total_needed && largest_block >= frame_needed) ? 
                "OK" : "INSUFFICIENT");
}
```

**If insufficient memory:** Consider:
1. Reduce baseline stream resolution to 640x480 VGA
2. Add aggressive memory cleanup before 5MP capture
3. Document as limitation and skip Phase 4

### Steps

#### 4.1 Add 5MP Sensor Mode

**Create:** `mipi_csi_camera/ov5647_5mp_mode.h`

```cpp
#pragma once

#include <esp_cam_sensor.h>

// 5MP mode configuration - extract from Espressif upstream
// https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/sensors/ov5647/ov5647.c

static const esp_cam_sensor_format_t OV5647_5MP_RAW10 = {
  .name = "OV5647_5MP_10FPS_RAW10",
  .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
  .port = ESP_CAM_SENSOR_MIPI_CSI,
  .xclk = 24000000,
  .width = 2592,
  .height = 1944,
  .regs = nullptr,  // Populate from upstream
  .regs_size = 0,
  .fps = 10,
  .isp_2_byte_align = 0,
  .mipi_info = {
    .mipi_clk = 1000000000 / 2,  // 500 MHz
    .lane_num = 2,
    .line_sync_en = false,
  },
};
```

**Action:** Extract register configuration from upstream driver.

#### 4.2 Update CaptureController for 5MP

In `capture_controller.h`, add:

```cpp
class CaptureController {
public:
  // ... existing methods ...
  
  // Add 5MP capture method
  CaptureResult capture_5mp(uint32_t timeout_ms = 10000);
  
private:
  // ... existing methods ...
  
  bool configure_sensor_5mp();
  bool allocate_buffers_5mp();
  
  esp_cam_sensor_format_t mode_5mp_;  // Add this
};
```

In `capture_controller.cpp` init():
```cpp
#include "ov5647_5mp_mode.h"

bool CaptureController::init(ESPVideoClass* capture_dev, PhotoStore* photo_store,
                              int video_fd, uint64_t boot_generation) {
  // ... existing code ...
  
  mode_5mp_ = OV5647_5MP_RAW10;
  
  Serial.printf("capture_ctrl: initialized 5MP mode=%ux%u\n",
                mode_5mp_.width, mode_5mp_.height);
  return true;
}
```

#### 4.3 Implement 5MP Capture Method

In `capture_controller.cpp`:

```cpp
CaptureResult CaptureController::capture_5mp(uint32_t timeout_ms) {
  CaptureResult result = {};
  result.success = false;
  result.restoration_succeeded = false;
  
  // Check memory before attempting
  size_t psram_free = ESP.getFreePsram();
  size_t min_required = (2592 * 1944 * 3 / 2) + (2 * 1024 * 1024);  // Frame + JPEG buffer
  
  if (psram_free < min_required) {
    Serial.printf("capture_ctrl: insufficient PSRAM free=%zu required=%zu\n",
                  psram_free, min_required);
    result.error_message = "insufficient memory";
    return result;
  }
  
  Serial.println("capture_ctrl: starting 5MP capture transaction");
  uint32_t txn_start = millis();
  
  // State: BaselineRunning → Stopping
  if (!stop_baseline()) {
    result.error_message = "failed to stop baseline";
    transition_to(ControllerState::Unavailable);
    return result;
  }
  
  // State: Stopping → SensorConfiguring
  if (!configure_sensor_5mp()) {
    result.error_message = "5MP sensor configuration failed";
    restore_baseline();
    return result;
  }
  
  // State: SensorConfiguring → BufferAllocating
  if (!allocate_buffers_5mp()) {
    result.error_message = "5MP buffer allocation failed";
    restore_baseline();
    return result;
  }
  
  // State: BufferAllocating → HighResReady → Capturing
  transition_to(ControllerState::HighResReady);
  transition_to(ControllerState::Capturing);
  
  ESPVideoBufferClass frame;
  if (!capture_frame(timeout_ms, frame)) {
    result.error_message = "5MP capture timeout or frame null";
    restore_baseline();
    return result;
  }
  
  result.capture_time_ms = millis() - txn_start;
  Serial.printf("capture_ctrl: 5MP frame captured in %ums, encoding JPEG\n", 
                result.capture_time_ms);
  
  // JPEG encode with higher quality for 5MP
  size_t jpeg_size = 0;
  uint8_t* jpeg_buf = encodeJpeg((uint8_t*)frame.getBufferPtr(),
                                  2592, 1944,
                                  PIXFORMAT_YUV420,  // Or RGB565 if Phase 3 skipped
                                  85,  // Higher quality for print-worthy images
                                  &jpeg_size);
  
  if (!jpeg_buf) {
    result.error_message = "5MP JPEG encoding failed";
    restore_baseline();
    return result;
  }
  
  // State: Capturing → Restoring
  transition_to(ControllerState::Restoring);
  result.restoration_succeeded = restore_baseline();
  
  if (result.restoration_succeeded) {
    // SUCCESS: Publish the photo
    photo_store_->publish(jpeg_buf, jpeg_size, 2592, 1944);
    result.success = true;
    Serial.printf("capture_ctrl: 5MP transaction complete, published photo\n");
  } else {
    // FAILURE: Do NOT publish, free buffer
    free(jpeg_buf);
    result.error_message = "5MP restoration failed";
    Serial.println("capture_ctrl: 5MP restoration failed, photo NOT published");
  }
  
  return result;
}

bool CaptureController::configure_sensor_5mp() {
  transition_to(ControllerState::SensorConfiguring);
  
  if (ioctl(video_fd_, VIDIOC_S_SENSOR_FMT, &mode_5mp_) != 0) {
    Serial.printf("capture_ctrl: 5MP sensor config failed, errno=%d\n", errno);
    return false;
  }
  
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = 2592;
  fmt.fmt.pix.height = 1944;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;  // Or RGB565
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  
  if (ioctl(video_fd_, VIDIOC_S_FMT, &fmt) != 0) {
    Serial.printf("capture_ctrl: 5MP video format failed, errno=%d\n", errno);
    return false;
  }
  
  return true;
}

bool CaptureController::allocate_buffers_5mp() {
  transition_to(ControllerState::BufferAllocating);
  
  if (!capture_dev_->startCapture()) {
    Serial.println("capture_ctrl: failed to start 5MP capture");
    return false;
  }
  
  return true;
}
```

#### 4.4 Update Background Trigger for 5MP

In main sketch `loop()`, add option to trigger 5MP captures:

```cpp
void loop() {
  // Background capture trigger with alternating resolution
  static uint32_t last_capture = 0;
  static bool use_5mp = false;
  uint32_t now = millis();
  
  if (now - last_capture > 30000) {
    if (use_5mp) {
      Serial.println("Background: triggering 5MP capture");
      CaptureResult result = capture_controller.capture_5mp();
      
      if (result.success) {
        Serial.printf("Background: 5MP capture succeeded, photo published\n");
      } else {
        Serial.printf("Background: 5MP capture failed - %s\n", 
                      result.error_message.c_str());
      }
      
      last_capture = now + result.capture_time_ms; // Interval + transaction
    } else {
      Serial.println("Background: triggering 1080p capture");
      CaptureResult result = capture_controller.capture_1080p();
      
      if (result.success) {
        Serial.printf("Background: 1080p capture succeeded, photo published\n");
      } else {
        Serial.printf("Background: 1080p capture failed - %s\n", 
                      result.error_message.c_str());
      }
      
      last_capture = now + result.capture_time_ms;
    }
    
    use_5mp = !use_5mp;  // Alternate between resolutions
  }
  
  delay(100);
}
```

**Note:** Cadence is `interval + transaction_time`, not just interval.

#### 4.5 Test 5MP Capture

```bash
# Wait for background 5MP capture trigger
# Watch serial log for state transitions

# Query latest photo
curl http://<board-ip>/photo/latest
# Should show: width=2592, height=1944

# Fetch 5MP JPEG
curl -o test_5mp.jpg http://<board-ip>/photo/<id>.jpg

# Verify dimensions
identify test_5mp.jpg  # Should show: 2592x1944

# Check file size (expect 400KB - 1.5MB depending on quality and scene)
ls -lh test_5mp.jpg
```

**Monitor serial output for:**
```
Background: triggering 5MP capture
capture_ctrl: starting 5MP capture transaction
capture_ctrl: state 1 → 2  (BaselineRunning → Stopping)
capture_ctrl: state 2 → 3  (Stopping → SensorConfiguring)
capture_ctrl: state 3 → 4  (SensorConfiguring → BufferAllocating)
capture_ctrl: state 4 → 5  (BufferAllocating → HighResReady)
capture_ctrl: state 5 → 6  (HighResReady → Capturing)
capture_ctrl: 5MP frame captured in 487ms, encoding JPEG
capture_ctrl: state 6 → 7  (Capturing → Restoring)
capture_ctrl: baseline restored successfully
capture_ctrl: state 7 → 1  (Restoring → BaselineRunning)
capture_ctrl: 5MP transaction complete, published photo
photo_store: published photo_id=N size=567890
Background: 5MP capture succeeded, photo published
```

**Verify:**
- Memory check passes before capture
- State transitions complete
- Frame capture within timeout (10s)
- JPEG encoding completes
- Baseline restoration succeeds
- Photo published only if restoration succeeds

#### 4.6 Performance Testing

```bash
# Test multiple captures via background trigger
# Monitor serial output for 5-10 5MP captures
# Record timing and success rates

# Or trigger manually if background cadence is too slow
# (Add manual trigger endpoint if needed)
```

**Record:**
- Capture time per frame (expect 500-1000ms)
- JPEG sizes (expect 400KB-1.5MB at quality 85)
- Memory errors or timeouts
- Restoration success rate (must be 100% for published photos)
- Baseline stream recovery time

#### 4.7 Document Phase 4 Results

Create `docs/phase4_results.md`:

```markdown
# Phase 4: 5MP Still Capture Results

## Pre-Check
- PSRAM available: X.X MiB
- 5MP frame size (format): X.X MiB
- Memory budget: [SUFFICIENT / INSUFFICIENT]

## Implementation Status
- 5MP sensor mode: [ADDED / FAILED]
- CaptureController 5MP method: [COMPLETE / INCOMPLETE]
- Background trigger updated: [YES / NO]
- Memory gate implemented: [YES / NO]

## Functional Testing
- Single 5MP capture: [SUCCESS / FAILED]
- Multiple captures: [X/5 succeeded]
- Baseline restoration: [WORKING / ISSUES]
- Memory stability: [STABLE / FRAGMENTATION / CRASHES]
- Publication only on restoration success: [VERIFIED / NOT_VERIFIED]

## Performance Metrics
- Average capture time: XXX ms
- Min/Max time: XXX / XXX ms
- Average JPEG size: XXX KB (quality 85)
- Success rate: X/5
- Restoration success rate: X/5 (must equal success rate)

## Image Quality
- Resolution verified: [YES / NO]
- Visual quality: [EXCELLENT / GOOD / POOR]
- Suitable for print: [YES / NO]
- Print size at 300 DPI: 8.6" x 6.5"

## Issues Encountered
- [List issues and resolutions]

## Limitations
- [Any discovered limitations]

## Phase 4 Complete
- [YES / NO]
```

**Exit Condition:** 5MP captures work reliably with CaptureController, photos published only when restoration succeeds, acceptable performance and quality, OR documented as unsupported with clear technical rationale.

---

## Phase 5: Integration and Polish

**Goal:** Add web UI for photo gallery, optimize performance, add diagnostics.

**CRITICAL:** This phase builds on the retained-photo API from Phase 2. The web UI queries `/photo/latest` for metadata and fetches JPEGs via `/photo/N.jpg`.

### Steps

#### 5.1 Create Landing Page with Photo Gallery

**Create:** `mipi_csi_camera/web/index.html`

```html
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-P4 Camera</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    
    body {
      background: #0A0D12;
      color: #E5E7EB;
      font-family: system-ui, -apple-system, sans-serif;
      padding: 2rem;
    }
    
    .container {
      max-width: 1200px;
      margin: 0 auto;
    }
    
    h1 {
      font-size: clamp(2rem, 5vw, 3rem);
      margin-bottom: 2rem;
      color: #F9FAFB;
    }
    
    .controls {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 1rem;
      margin-bottom: 2rem;
    }
    
    .card {
      background: #161D2B;
      border-radius: 12px;
      padding: 1.5rem;
    }
    
    .card h2 {
      font-size: 1.125rem;
      margin-bottom: 1rem;
      color: #F3F4F6;
    }
    
    button {
      width: 100%;
      background: #38BDF8;
      color: #0A0D12;
      border: none;
      padding: 0.75rem 1.5rem;
      border-radius: 999px;
      font-weight: 600;
      font-size: 1rem;
      cursor: pointer;
      transition: all 0.2s;
    }
    
    button:hover {
      background: #0EA5E9;
      transform: translateY(-1px);
    }
    
    button:active {
      transform: translateY(0);
    }
    
    button:disabled {
      background: #334155;
      color: #64748B;
      cursor: not-allowed;
      transform: none;
    }
    
    .status {
      margin-top: 0.5rem;
      font-size: 0.875rem;
      color: #94A3B8;
    }
    
    .photo-container {
      background: #161D2B;
      border-radius: 12px;
      overflow: hidden;
      margin-bottom: 2rem;
    }
    
    .photo-container img {
      width: 100%;
      height: auto;
      display: block;
    }
    
    .photo-info {
      padding: 1rem;
      font-size: 0.875rem;
      color: #94A3B8;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>ESP32-P4 Camera</h1>
    
    <div class="controls">
      <div class="card">
        <h2>Latest Photo</h2>
        <button onclick="refreshPhoto()">Refresh Photo</button>
        <div class="status" id="status-latest"></div>
      </div>
      
      <div class="card">
        <h2>System Info</h2>
        <div class="status" id="system-info">Loading...</div>
      </div>
    </div>
    
    <div class="photo-container" id="photo-container" style="display:none;">
      <img id="photo-img" src="" alt="Latest capture">
      <div class="photo-info" id="photo-info"></div>
    </div>
  </div>
  
  <script>
    let currentPhotoId = null;
    
    async function refreshPhoto() {
      const statusEl = document.getElementById('status-latest');
      statusEl.textContent = 'Checking for new photo...';
      
      try {
        // Fetch metadata
        const metaResponse = await fetch('/photo/latest');
        if (!metaResponse.ok) {
          throw new Error('No photo available');
        }
        
        const meta = await metaResponse.json();
        
        // Check if this is a new photo
        if (meta.photo_id === currentPhotoId) {
          statusEl.textContent = 'Already showing latest photo';
          return;
        }
        
        // Fetch JPEG with conditional request
        const photoUrl = `/photo/${meta.photo_id}.jpg`;
        const headers = currentPhotoId ? 
          {'If-None-Match': `"${currentPhotoId}"`} : {};
        
        const photoResponse = await fetch(photoUrl, { headers });
        
        if (photoResponse.status === 304) {
          statusEl.textContent = 'Photo unchanged';
          return;
        }
        
        if (!photoResponse.ok) {
          throw new Error('Failed to fetch photo');
        }
        
        const blob = await photoResponse.blob();
        const url = URL.createObjectURL(blob);
        
        // Display photo
        document.getElementById('photo-img').src = url;
        document.getElementById('photo-container').style.display = 'block';
        
        // Update info
        const timestamp = new Date(meta.timestamp / 1000);
        document.getElementById('photo-info').innerHTML = `
          Photo ID: ${meta.photo_id} | 
          ${meta.width}×${meta.height} | 
          ${(meta.size / 1024).toFixed(0)} KB | 
          ${timestamp.toLocaleString()}
        `;
        
        currentPhotoId = meta.photo_id;
        statusEl.textContent = `Showing photo #${meta.photo_id}`;
        statusEl.style.color = '#6EE7B7';
        
      } catch (error) {
        statusEl.textContent = `Error: ${error.message}`;
        statusEl.style.color = '#F87171';
      }
    }
    
    async function updateSystemInfo() {
      try {
        const response = await fetch('/photo/latest');
        if (response.ok) {
          const meta = await response.json();
          document.getElementById('system-info').innerHTML = `
            Latest Photo: #${meta.photo_id}<br>
            Resolution: ${meta.width}×${meta.height}<br>
            Size: ${(meta.size / 1024).toFixed(0)} KB
          `;
        }
      } catch (error) {
        document.getElementById('system-info').textContent = 'No photos yet';
      }
    }
    
    // Auto-refresh photo every 30 seconds
    setInterval(refreshPhoto, 30000);
    
    // Update system info every 5 seconds
    setInterval(updateSystemInfo, 5000);
    
    // Initial load
    refreshPhoto();
    updateSystemInfo();
  </script>
</body>
</html>
```

**Key features:**
- Polls `/photo/latest` for metadata
- Uses conditional requests with If-None-Match
- Shows photo ID, resolution, size, timestamp
- Auto-refreshes every 30 seconds

#### 5.2 Serve Landing Page with esp_http_server

In main sketch or `photo_api.cpp`:

```cpp
// Serve root page
esp_err_t handle_root(httpd_req_t* req) {
  const char* html = R"====(
    <!DOCTYPE html>
    <!-- Embed the HTML from index.html here -->
    <!-- Or use SPIFFS/LittleFS if available -->
  )====";
  
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html, strlen(html));
  return ESP_OK;
}

// Register in setup()
httpd_uri_t root_uri = {
  .uri = "/",
  .method = HTTP_GET,
  .handler = handle_root,
  .user_ctx = nullptr
};

httpd_register_uri_handler(http_server, &root_uri);
```

**Alternative:** If SPIFFS/LittleFS is available, serve file directly:
```cpp
// Load from filesystem
File file = SPIFFS.open("/web/index.html", "r");
// Stream to response
```

#### 5.3 Add Diagnostics Endpoint

```cpp
esp_err_t handle_diagnostics(httpd_req_t* req) {
  char json[512];
  snprintf(json, sizeof(json),
           "{"
           "\"heap_free\":%u,"
           "\"psram_total\":%u,"
           "\"psram_free\":%u,"
           "\"largest_free\":%u,"
           "\"controller_state\":%d,"
           "\"latest_photo_id\":%u,"
           "\"uptime_ms\":%lu"
           "}",
           ESP.getFreeHeap(),
           ESP.getPsramSize(),
           ESP.getFreePsram(),
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
           (int)capture_controller.get_state(),
           photo_store.get_latest_id(),
           millis());
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, strlen(json));
  return ESP_OK;
}

// Register endpoint
httpd_uri_t diag_uri = {
  .uri = "/diag",
  .method = HTTP_GET,
  .handler = handle_diagnostics,
  .user_ctx = nullptr
};

httpd_register_uri_handler(http_server, &diag_uri);
```

#### 5.4 Update README

Add usage documentation to `README.md`:

````markdown
## High-Resolution Still Capture

### Retained-Photo API

**Get Latest Photo Metadata**
```bash
curl http://<board-ip>/photo/latest
```
Returns: `{"photo_id":123,"width":1920,"height":1080,"size":234567,"timestamp":1234567890}`

**Fetch Photo JPEG**
```bash
curl -o photo.jpg http://<board-ip>/photo/123.jpg
```

**Conditional Request (ETag)**
```bash
curl -H 'If-None-Match: "123"' http://<board-ip>/photo/123.jpg
# Returns 304 Not Modified if photo_id matches
```

**Web Interface**
```
http://<board-ip>/
```

**Diagnostics**
```bash
curl http://<board-ip>/diag
```

### Architecture

- **Background capture**: Automatic at configured interval
- **Retained-photo API**: Photos stored until replaced by newer capture
- **Photo ID**: Increments only on successful publication (restoration must succeed)
- **Baseline restoration**: Automatic after each capture transaction
- **Timeout protection**: 5s for 1080p, 10s for 5MP
- **State machine**: CaptureController manages all transitions

### Performance

- **1080p:** ~300-500ms capture time, 150-300 KB JPEG
- **5MP:** ~500-1000ms capture time, 400-1500 KB JPEG
- **Format:** RGB565 (or YUV420 if available)
- **Quality:** 80 for 1080p, 85 for 5MP

### Technical Details

- Baseline stream: 800x800 @ 10 FPS (RGB565, MJPEG quality 50)
- High-res capture uses temporary mode switching
- Automatic baseline restoration after capture
- Photos published only if restoration succeeds
- Timeout mechanism prevents indefinite hangs
- PhotoStore with mutex-protected acquire/release
- Background cadence: interval + transaction_time
````

#### 5.5 Final Integration Testing

**Test checklist:**

1. **Baseline stream**
   - [ ] Starts automatically on boot
   - [ ] Accessible via RTSP at rtsp://<ip>:554/
   - [ ] Stable 10 FPS
   - [ ] Continues during background captures

2. **Background capture trigger**
   - [ ] Triggers automatically at configured interval
   - [ ] Logs complete state machine transitions
   - [ ] Photos published only on successful restoration
   - [ ] Cadence is interval + transaction_time

3. **Retained-photo API**
   - [ ] /photo/latest returns metadata
   - [ ] /photo/N.jpg returns JPEG
   - [ ] Conditional requests (ETag) work correctly
   - [ ] 304 Not Modified when photo unchanged
   - [ ] Photo ID increments only on publication

4. **1080p capture**
   - [ ] Background trigger works
   - [ ] State transitions complete correctly
   - [ ] Timeout protection working
   - [ ] Baseline restores automatically
   - [ ] Photo published only if restoration succeeds

5. **5MP capture**
   - [ ] Background trigger works
   - [ ] Memory check passes
   - [ ] State transitions complete
   - [ ] No memory errors
   - [ ] Baseline restores automatically
   - [ ] Photo published only if restoration succeeds

6. **Web interface**
   - [ ] Landing page loads at http://<ip>/
   - [ ] Latest photo displays correctly
   - [ ] Auto-refresh works (30s interval)
   - [ ] Metadata shows correct information
   - [ ] Conditional requests reduce bandwidth

7. **CaptureController state machine**
   - [ ] All transitions logged
   - [ ] Unavailable state on critical failure
   - [ ] Recovery to BaselineRunning on success
   - [ ] Proper error handling in each state

8. **PhotoStore thread safety**
   - [ ] Mutex-protected acquire works
   - [ ] Reference counting prevents premature free
   - [ ] No memory leaks over extended operation
   - [ ] Old photos cleaned up correctly

9. **Publication policy**
   - [ ] Photo published only if restoration succeeds
   - [ ] Photo ID does NOT increment on restoration failure
   - [ ] JPEG buffer freed if not published

10. **Stability**
    - [ ] 50 mixed captures (25x 1080p + 25x 5MP) without crash
    - [ ] Memory stable over time (no leaks or fragmentation)
    - [ ] No baseline stream degradation
    - [ ] System recovers from timeout scenarios

#### 5.6 Document Phase 5 Results

Create `docs/phase5_results.md`:

```markdown
# Phase 5: Integration and Polish Results

## Web Interface
- Landing page: [COMPLETE / INCOMPLETE]
- Retained-photo API integration: [WORKING / ISSUES]
- Auto-refresh functionality: [WORKING / ISSUES]
- Conditional request support: [WORKING / ISSUES]

## API Validation
- /photo/latest metadata: [WORKING / ISSUES]
- /photo/N.jpg JPEG fetch: [WORKING / ISSUES]
- ETag conditional requests: [WORKING / ISSUES]
- 304 Not Modified responses: [WORKING / ISSUES]

## Stability Testing
- 50 capture stress test: [PASSED / FAILED]
- Longest stable run: XXX captures
- Memory stability: [STABLE / ISSUES]
- PhotoStore thread safety: [VERIFIED / ISSUES]
- Baseline stream continuity: [MAINTAINED / INTERRUPTED]

## State Machine Validation
- All transitions logged correctly: [YES / NO]
- Recovery from failures: [WORKING / ISSUES]
- Unavailable state handling: [CORRECT / ISSUES]

## Publication Policy Validation
- Photos published only on restoration success: [VERIFIED / NOT_VERIFIED]
- Photo ID increments correctly: [VERIFIED / NOT_VERIFIED]
- JPEG freed when not published: [VERIFIED / NOT_VERIFIED]

## Documentation
- README updated: [YES / NO]
- API documented: [YES / NO]
- Usage examples: [YES / NO]
- Architecture documented: [YES / NO]

## Known Issues
- [List any remaining issues]

## Project Complete
- [YES / NO]
```

**Exit Condition:** All components working with retained-photo API, CaptureController state machine validated, PhotoStore thread-safe, web interface functional, publication policy correct, stability validated, documentation complete.

---

## Troubleshooting Guide

### Common Issues

#### Issue: Capture Times Out

**Symptoms:**
- Serial log shows "capture timeout"
- HTTP returns 503
- Recovery initiates

**Debugging steps:**
1. Check sensor mode was applied: Look for `mode_mgr: applied format` in serial
2. Verify pipeline reinitialization: `still_capture: reinitializing pipeline`
3. Check PSRAM availability: Add memory logging
4. Test with longer timeout: Increase from 5s to 10s

**Likely causes:**
- Incorrect sensor register configuration
- ISP pipeline issue with high-res mode
- Insufficient frame buffers allocated

#### Issue: JPEG Encoding Fails

**Symptoms:**
- Frame captured successfully
- Error: "JPEG encoding failed"

**Debugging steps:**
1. Check PSRAM for JPEG buffer: Need ~2 MiB free
2. Verify pixel format matches encoder expectation
3. Test with lower quality setting (50 instead of 80)
4. Check jpeg_encoder supports input format

**Likely causes:**
- Memory exhausted during encoding
- Wrong pixel format passed to encoder
- JPEG encoder buffer allocation failed

#### Issue: Baseline Stream Doesn't Restore

**Symptoms:**
- High-res capture succeeds
- RTSP stream doesn't resume
- Serial shows stuck in capture mode

**Debugging steps:**
1. Check mode switch back to baseline: `mode_mgr: switching to mode=0`
2. Verify `startCapture()` succeeds after restore
3. Check for pipeline state corruption
4. Try manual reset via `/diag/reset` endpoint

**Likely causes:**
- Mode switch partially failed
- Pipeline not fully cleaned up
- Video device in error state

#### Issue: Memory Fragmentation

**Symptoms:**
- First few captures work
- Later captures fail with memory errors
- PSRAM free decreases over time

**Debugging steps:**
1. Add memory logging before/after each capture
2. Check for JPEG buffer leaks: Ensure `free()` is called
3. Monitor heap fragmentation: `heap_caps_get_largest_free_block()`
4. Look for unclosed file descriptors

**Likely causes:**
- JPEG buffers not freed properly
- Frame buffers not released
- Heap fragmentation from mixed allocations

### Recovery Procedures

#### Full System Reset

If the system gets into an unrecoverable state:

```cpp
// Add emergency reset endpoint
server.on("/diag/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Resetting in 2 seconds...");
  delay(2000);
  ESP.restart();
});
```

#### Force Baseline Mode

If stuck in capture mode:

```cpp
// Add force baseline endpoint
server.on("/diag/force_baseline", HTTP_GET, [](AsyncWebServerRequest *request) {
  Serial.println("DIAG: forcing baseline mode");
  
  capture_dev.stopCapture();
  delay(100);
  
  mode_mgr.switchMode(CameraMode::BASELINE);
  delay(100);
  
  capture_dev.startCapture();
  
  request->send(200, "text/plain", "Baseline mode restored");
});
```

---

## Implementation Checklist

Use this checklist to track progress through all phases:

### Phase 0: Hardware Baseline
- [ ] Flash current firmware to hardware
- [ ] Capture baseline serial log
- [ ] Test RTSP stream
- [ ] Document hardware validation results
- [ ] **Exit condition met:** Baseline operation confirmed

### Phase 1: Format Support
- [ ] Validate RGB565 high-res support (REQUIRED)
- [ ] Probe YUV420 format support (optional)
- [ ] Add 1080p sensor mode descriptor
- [ ] Test 1080p mode application
- [ ] Add VGA mode if needed
- [ ] Document Phase 1 results
- [ ] **Exit condition met:** RGB565 confirmed, 1080p mode applies

### Phase 1.5: Timeout Mechanism (BLOCKER)
- [ ] Choose approach (Tier 1: ESP-Video patching REQUIRED, Tier 2: Watchdog fallback)
- [ ] Tier 1: Vendor ESP-Video and patch VFS
- [ ] Tier 2: Implement watchdog (only if Tier 1 impossible)
- [ ] Test timeout detection
- [ ] Document implementation and tier choice
- [ ] **Exit condition met:** Timeout mechanism working (Tier 1 preferred)

### Phase 2: 1080p Capture with Retained-Photo API
- [ ] Create PhotoStore with mutex-protected acquire
- [ ] Create CaptureController with state machine
- [ ] Implement retained-photo API with esp_http_server
- [ ] Add background capture trigger
- [ ] Test /photo/latest metadata endpoint
- [ ] Test /photo/N.jpg JPEG fetch
- [ ] Test conditional requests (ETag)
- [ ] Verify do-NOT-publish-if-restoration-fails policy
- [ ] Test multiple captures
- [ ] Measure performance
- [ ] Document Phase 2 results
- [ ] **Exit condition met:** 1080p working with retained-photo API, PhotoStore thread-safe

### Phase 3: YUV420 Optimization (OPTIONAL)
- [ ] Update CaptureController for YUV420
- [ ] Update JPEG encoder call
- [ ] Test YUV420 capture
- [ ] Measure memory savings
- [ ] Document Phase 3 results (or skip reason)
- [ ] **Exit condition met:** YUV420 validated or documented skip

### Phase 4: 5MP Capture
- [ ] Check memory budget with runtime measurement
- [ ] Add 5MP sensor mode
- [ ] Update CaptureController for 5MP
- [ ] Implement capture_5mp method
- [ ] Update background trigger for 5MP
- [ ] Test 5MP capture
- [ ] Performance testing
- [ ] Document Phase 4 results
- [ ] **Exit condition met:** 5MP working with CaptureController or documented limitation

### Phase 5: Integration
- [ ] Create landing page with retained-photo API integration
- [ ] Serve web interface with esp_http_server
- [ ] Add diagnostics endpoint
- [ ] Update README with retained-photo API usage
- [ ] Complete integration testing (all 10 test categories)
- [ ] Verify CaptureController state machine
- [ ] Verify PhotoStore thread safety
- [ ] Verify publication policy
- [ ] Document Phase 5 results
- [ ] **Exit condition met:** Complete system validated with correct architecture

---

## Code Organization

Final file structure after all phases:

```
mipi_csi_camera/
├── mipi_csi_camera.ino           # Main sketch with esp_http_server
├── jpeg_encoder.h                # Existing JPEG encoder
├── jpeg_encoder.cpp
├── photo_store.h                 # Phase 2: PhotoStore with mutex-protected acquire
├── photo_store.cpp
├── capture_controller.h          # Phase 2: Unified CaptureController state machine
├── capture_controller.cpp
├── photo_api.h                   # Phase 2: Retained-photo API endpoints
├── photo_api.cpp
├── capture_watchdog.h            # Phase 1.5: Timeout (if Tier 2 fallback)
├── capture_watchdog.cpp
├── ov5647_modes.h                # Phase 1: 1080p sensor mode descriptor
├── ov5647_5mp_mode.h             # Phase 4: 5MP mode descriptor
├── lib/
│   └── ESP_Video_Patched/        # Phase 1.5: Vendored ESP-Video (Tier 1 required)
│       ├── PATCHES.md
│       └── [ESP-Video source]
└── web/
    └── index.html                # Phase 5: Web interface (optional separate file)

docs/
├── OV5647_HIGH_RES_STILL_CAPTURE_PLAN_V2.md  # Architecture (v2.2)
├── OV5647_HIGH_RES_CAPTURE_AI_EXECUTION_GUIDE.md  # This document (v2.2)
├── phase0_results.md
├── phase0_hardware_baseline.log
├── phase1_results.md
├── phase1_5_timeout_results.md
├── phase2_results.md
├── phase3_results.md
├── phase4_results.md
└── phase5_results.md
```

**Key architectural components:**
- **PhotoStore**: Thread-safe photo storage with mutex-protected acquire/release
- **CaptureController**: Unified state machine managing entire capture transaction
- **PhotoAPI**: Retained-photo API with metadata and conditional requests
- **esp_http_server**: ESP-IDF native HTTP server (not AsyncWebServer)

---

## AI Implementation Guidelines

### General Principles

1. **Read before acting:** Always read existing code to understand patterns before adding new code.

2. **Follow conventions:** Match existing code style, naming, and organization.

3. **Validate assumptions:** Don't assume hardware capabilities—probe and verify.

4. **Document results:** Create result documents after each phase to track decisions and outcomes.

5. **Incremental testing:** Test after each significant change, don't wait until the end.

6. **Graceful degradation:** If a feature doesn't work (e.g., YUV420 unavailable), document why and continue.

### Code Style Guidelines

**Naming conventions (based on existing codebase):**
- Classes: `PascalCase` (e.g., `ModeManager`)
- Methods: `camelCase` (e.g., `switchMode()`)
- Private members: `snake_case_` with trailing underscore (e.g., `current_mode_`)
- Constants: `UPPER_SNAKE_CASE` or `PascalCase` for static const structs
- File names: `snake_case.h/cpp`

**Serial output format:**
```cpp
Serial.printf("module_name: action status=value key=value\n");
// Examples:
// mode_mgr: switching to mode=1
// still_capture: frame received in 234ms
// memory milestone=boot heap_free=123456
```

**Error handling:**
```cpp
if (!operation()) {
  Serial.println("module: operation failed");
  // Attempt recovery
  return false;
}
```

**Memory logging:**
```cpp
Serial.printf("memory %s heap_free=%u psram_free=%u\n",
              label, ESP.getFreeHeap(), ESP.getFreePsram());
```

### Testing Strategy

**For each phase:**

1. **Compile test:** Ensure code compiles without errors
2. **Flash test:** Upload to hardware successfully
3. **Boot test:** System boots and initializes
4. **Function test:** Feature works as designed
5. **Stability test:** Multiple iterations without failure
6. **Integration test:** Doesn't break existing features

**When issues occur:**

1. Check serial output for error messages
2. Add diagnostic logging if needed
3. Test with simpler configuration first
4. Isolate the failing component
5. Document the issue in phase results
6. Attempt recovery or fallback approach

### Documentation Requirements

**Each phase result document must include:**

- Implementation status (complete/incomplete)
- Test results (pass/fail with details)
- Performance metrics (times, sizes, rates)
- Issues encountered and resolutions
- Decisions made and rationale
- Ready for next phase (yes/no with explanation)

**Use concrete values, not placeholders:**
- Good: "Average capture time: 287ms"
- Bad: "Average capture time: XXX ms"

**Include actual serial log excerpts:**
```
mode_mgr: switching to mode=1
mode_mgr: applied format 1920x1080 fourcc=RGB565
still_capture: frame received in 287ms
```

### Working with Hardware Constraints

**Memory management:**
- Always check available PSRAM before large allocations
- Free JPEG buffers immediately after sending HTTP response
- Log memory state before/after major operations
- Watch for fragmentation over multiple captures

**Timing considerations:**
- Mode switches take 100-200ms
- Frame capture varies: 50-500ms depending on resolution
- JPEG encoding: 50-200ms depending on size
- Total 1080p capture: expect 300-500ms
- Total 5MP capture: expect 500-1000ms

**Single-pipeline constraint:**
- Only one resolution active at a time
- Must stop baseline before high-res capture
- Must restore baseline after capture
- Mode switches require pipeline re-initialization

### Code Review Checklist

Before marking a phase complete, verify:

- [ ] Code compiles without warnings
- [ ] All memory allocations have corresponding frees
- [ ] Error cases are handled (not just success path)
- [ ] Serial logging provides useful diagnostics
- [ ] No hardcoded magic numbers (use named constants)
- [ ] Functions have reasonable length (<100 lines)
- [ ] No code duplication (extract common patterns)
- [ ] Comments explain *why*, not *what*
- [ ] Resource cleanup in error paths
- [ ] Thread-safety considered for shared state

---

## Success Criteria

The implementation is complete when:

### Functional Requirements
- [x] Baseline 800x800 stream works continuously
- [ ] Background capture trigger works automatically
- [ ] 1080p still captures work reliably
- [ ] 5MP still captures work reliably (or limitation documented)
- [ ] Retained-photo API provides metadata and JPEG access
- [ ] Photos published only when restoration succeeds
- [ ] Baseline stream automatically restores after capture
- [ ] Timeout mechanism prevents indefinite hangs
- [ ] Web interface provides user-friendly access

### Architectural Requirements
- [ ] CaptureController unified state machine (not split ModeManager)
- [ ] PhotoStore with mutex-protected acquire (no use-after-free)
- [ ] esp_http_server (not AsyncWebServer)
- [ ] Retained-photo API (not synchronous capture endpoints)
- [ ] Do-NOT-publish-if-restoration-fails policy enforced
- [ ] Photo ID increments only on successful publication
- [ ] ESP-Video VFS patching (Tier 1) or watchdog (Tier 2 with limitation documented)
- [ ] RGB565 primary format validated, YUV420 optional

### Performance Requirements
- [ ] 1080p capture completes in <1 second
- [ ] 5MP capture completes in <2 seconds
- [ ] Baseline stream maintains ~10 FPS
- [ ] No memory leaks over 50+ captures
- [ ] System stable for continuous operation
- [ ] Background cadence: interval + transaction_time

### Quality Requirements
- [ ] 1080p images are sharp and well-exposed
- [ ] 5MP images are suitable for print (300 DPI = 8.6" x 6.5")
- [ ] JPEG compression doesn't introduce severe artifacts
- [ ] Color reproduction is acceptable

### Documentation Requirements
- [ ] All phases documented with results
- [ ] README includes retained-photo API usage
- [ ] Known limitations documented
- [ ] Architecture components documented
- [ ] Troubleshooting guide complete

---

## Emergency Contacts / Resources

**Reference Documentation:**
- ESP-IDF Video API: `esp_video_ioctl.h` definitions
- OV5647 Datasheet: Sensor registers and modes
- Espressif OV5647 Driver: https://github.com/espressif/esp-video-components
- V4L2 API Reference: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html

**Key Source Files:**
- Main sketch: `mipi_csi_camera/mipi_csi_camera.ino`
- JPEG encoder: `mipi_csi_camera/jpeg_encoder.cpp`
- ESP-Video library: Arduino packages or project libraries folder

**Diagnostic Commands:**
```bash
# View serial output
screen /dev/tty.usbserial-* 115200

# Test RTSP stream
python3 tools/rtsp_viewer.py rtsp://<ip>:554/

# Test HTTP endpoint
curl -v http://<ip>/capture/1080p -o test.jpg

# Check image
identify test.jpg
file test.jpg
```

---

## Appendix: Worked Example Timeline

Realistic timeline for AI implementation:

**Session 1 (2-3 hours):**
- Phase 0: Hardware validation (30 min)
- Phase 1: Format support validation (1 hour)
- Phase 1.5: Start timeout mechanism (1 hour)

**Session 2 (2-3 hours):**
- Phase 1.5: Complete and test timeout mechanism (1 hour)
- Phase 2: Implement 1080p capture (2 hours)

**Session 3 (2-3 hours):**
- Phase 2: Test and validate 1080p (1 hour)
- Phase 3: YUV420 optimization (1 hour)
- Phase 4: Start 5MP implementation (30 min)

**Session 4 (2-3 hours):**
- Phase 4: Complete and test 5MP (1.5 hours)
- Phase 5: Integration and polish (1.5 hours)

**Session 5 (1-2 hours):**
- Final testing and documentation
- Troubleshooting any issues

**Total estimated time: 10-15 hours** across 5 work sessions.

---

## Version History

**Version 2.2** (2026-08-18)
- Complete alignment with Plan V2.2 final architecture review
- Replaced ModeManager+StillCaptureHandler split with unified CaptureController
- Implemented PhotoStore with mutex-protected acquire() (prevents use-after-free)
- Changed to esp_http_server (ESP-IDF native, not AsyncWebServer)
- Implemented retained-photo API (metadata polling + conditional requests)
- Added do-NOT-publish-if-restoration-fails policy
- Photo ID increments only on successful publication
- Clarified RGB565 as primary format, YUV420 as optional optimization
- ESP-Video VFS patching as Tier 1 (required), watchdog as Tier 2 (degraded fallback)
- Background cadence: interval + transaction_time (not interval alone)
- Complete CaptureController state machine with explicit transitions
- Updated all phases with correct architecture
- Removed warning banner - guide now aligned with Plan V2.2

**Version 2.0** (2026-08-18)
- Initial execution guide based on Plan V2.1
- All phases documented
- Troubleshooting guide included
- AI implementation guidelines added

---

## Document End

This execution guide is complete and aligned with `OV5647_HIGH_RES_STILL_CAPTURE_PLAN_V2.md` v2.2.

Good luck with the implementation!

