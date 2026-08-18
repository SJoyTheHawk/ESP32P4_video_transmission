# Execution Guide V2.2 - Critical Corrections

**Date:** 2026-08-18  
**Status:** BLOCKING - Guide must not be followed as-is for Plan V2.2

## ⚠️ DO NOT IMPLEMENT FROM CURRENT GUIDE WITHOUT THESE CORRECTIONS

The current Execution Guide (v2.0) has architectural mismatches with Plan V2.2 that will
result in incorrect implementation. This document lists corrections that MUST be applied.

---

## 1. Format Priority (Phase 1)

### ❌ Current Guide Says:
- Probe YUV420 support first
- If YUV420 not found, fall back to RGB565
- YUV420 is the primary implementation path

### ✅ Plan V2.2 Requires:
- **RGB565 is the primary implementation path**
- YUV420 is an **optional optimization** attempted only after RGB565 validation
- RGB565 not found = **cannot proceed** (critical error)
- Sequence: RGB565 capture → encode → HTTP → test end-to-end → **then** try YUV420

**Rationale:** RGB565 has guaranteed JPEG encoder support. YUV420 requires ISP pipeline
analysis and potential conversion logic that may not be available.

---

## 2. Timeout Mechanism (Phase 1.5)

### ❌ Current Guide Says:
- Offers watchdog task as alternative to ESP-Video patching
- Presents both options as equivalent
- Includes `CaptureWatchdog` class implementation

### ✅ Plan V2.2 Requires:
- **ESP-Video patching is Tier 1 (required approach)**
- Watchdog task is **Tier 2 (degraded fallback only)**
- Patching instructions must be step-by-step, not optional
- Phase gate: Tier 1 working OR documented reason why Tier 2 is necessary

**Rationale:** `dqbuf()` timeout support is a blocking gate. Watchdog adds complexity
and only catches hanging reads (not stuck driver states).

**Required additions:**
1. Locate ESP-Video component source directory
2. Find VFS ioctl handler for `/dev/video0`
3. Add timeout parameter to `dqbuf()` implementation
4. Rebuild ESP-Video component
5. Test with deterministic delay injection

---

## 3. JPEG Allocator Verification (NEW PHASE 1.5b)

### ❌ Current Guide Says:
- Nothing - this phase is missing entirely

### ✅ Plan V2.2 Requires:
- **Complete verification phase before encoder integration**
- Must identify matching allocator/deallocator pair
- Create `JpegOutputBuffer` adapter class
- Test allocate/release cycles without leaks

**Blocking:** Cannot proceed to Phase 2 encoder integration without verified allocator.

**Required steps:**
1. Locate `jpeg_alloc_encoder_mem()` in ESP-IDF headers
2. Find paired deallocator (heap_caps_free? jpeg_free? regular free?)
3. Document allocator heap requirements (MALLOC_CAP_SPIRAM? MALLOC_CAP_DMA?)
4. Create adapter wrapper
5. Test 100 alloc/free cycles and verify no growth

---

## 4. Controller Architecture (Phase 2)

### ❌ Current Guide Says:
- Create `ModeManager` class
- Create separate `StillCaptureHandler` class
- Two classes coordinate camera pipeline control

### ✅ Plan V2.2 Requires:
- **Single unified `CaptureController` class**
- Complete state machine with explicit transitions
- States: Uninitialized, BaselineRunning, Stopping, SensorConfiguring, 
  BufferAllocating, HighResReady, Capturing, Restoring, Unavailable
- Each transition has success/failure paths documented

**Rationale:** Split ownership creates race conditions. Single controller owns the
camera pipeline lifecycle completely.

**Complete state machine required:**
```
BaselineRunning
  → capture_1080p() → Stopping
    → success → SensorConfiguring
      → success → BufferAllocating
        → success → HighResReady → start_capture() → Capturing
          → success → Restoring → success → BaselineRunning
          → failure → Restoring → success → BaselineRunning (previous photo retained)
          → failure → Restoring → failure → Unavailable (do NOT publish)
        → failure → Restoring
      → failure → Restoring
    → failure → BaselineRunning (abort)
```

---

## 5. PhotoStore Race Condition (Phase 3)

### ❌ Current Guide Says:
```cpp
PhotoBlob* blob = store.latest;
if (blob) {
  blob->acquire();  // RACE: blob could be released here
  return blob;
}
```

### ✅ Plan V2.2 Requires:
```cpp
class PhotoStore {
  SemaphoreHandle_t mutex_;
  PhotoBlob* current_;
  
  PhotoBlob* acquire_latest() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    PhotoBlob* result = current_;
    if (result) {
      result->ref_count.fetch_add(1, std::memory_order_acquire);
    }
    xSemaphoreGive(mutex_);
    return result;
  }
};
```

**Rationale:** Without mutex protection, the blob can be released between the null check
and acquire call, causing use-after-free.

**Critical:** Acquire must be mutex-protected. Release can be atomic-only.

---

## 6. HTTP Server Framework (Phase 4)

### ❌ Current Guide Says:
- All code examples use `AsyncWebServer`
- Async response patterns throughout

### ✅ Plan V2.2 Requires:
- **Use `esp_http_server` (ESP-IDF native) throughout**
- No AsyncWebServer references

**Wrong:**
```cpp
#include <ESPAsyncWebServer.h>
AsyncWebServer server(80);
server.on("/path", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "OK");
});
```

**Right:**
```cpp
#include <esp_http_server.h>
httpd_handle_t server = nullptr;
httpd_uri_t uri_config = {
  .uri = "/path",
  .method = HTTP_GET,
  .handler = handler_function,
};
httpd_register_uri_handler(server, &uri_config);
```

---

## 7. API Pattern (Phase 4)

### ❌ Current Guide Says:
- `POST /capture/1080p` returns JPEG immediately (synchronous)
- Client waits for entire capture + encode cycle

### ✅ Plan V2.2 Requires:
- **Retained-photo API with metadata polling**
- `POST /api/photo/capture` returns `202 Accepted` immediately
- `GET /api/photo/metadata` polls for completion
- `GET /api/photo/latest.jpg?id=<photo_id>` downloads when ready

**API contract:**
- `photo_id` increments **only** on successful publication
- Previous photo remains available during capture
- Previous photo remains available if refresh fails (but restores baseline)
- If restoration fails, do **NOT** publish, camera enters Unavailable state
- 410 Gone for stale photo_id
- 503 Service Unavailable when camera is Unavailable state

---

## 8. Publication-on-Failure Policy (Phase 4)

### ❌ Current Guide Says:
- Publish captured photo even if restoration fails
- OR: Unclear what happens on restoration failure

### ✅ Plan V2.2 Requires:
- **Do NOT publish if restoration fails**
- Sequence:
  1. Capture succeeds
  2. Encode succeeds
  3. Restoration fails → camera enters **Unavailable** state
  4. Previous photo still available (not replaced)
  5. HTTP API returns 503 Service Unavailable for new requests
  6. Require manual intervention or reboot

**Rationale:** Publishing a high-res photo when baseline cannot be restored would leave
RTSP stream broken indefinitely. Better to keep old photo + signal unavailability.

---

## 9. RTSP Keepalive Approach (Phase 5)

### ❌ Current Guide Says:
- Implement RTCP sender reports or OPTIONS keepalive
- Prescriptive - add keepalive code during implementation

### ✅ Plan V2.2 Requires:
- **Measurement-based approach**
- Sequence:
  1. Implement on-demand capture without keepalive
  2. Measure media gap with VLC, ffmpeg, GStreamer
  3. Document whether clients time out or reconnect successfully
  4. **Only** add keepalive if measurement shows it's necessary

**Rationale:** Keepalive adds complexity. Many RTSP clients handle gaps correctly.
Don't add code without proven need.

---

## 10. Background Cadence Expectations (Phase 6)

### ❌ Current Guide Says:
- "Actual cadence matches configured interval"
- "100 captures at 1-second intervals completes in ~100 seconds"

### ✅ Plan V2.2 Requires:
- "Actual cadence = interval + transaction duration"
- "1-second interval with 1.5s transaction duration = 2.5s between capture starts"
- Interval is measured from **completion** of previous capture to **start** of next
- Captures never overlap
- Missed intervals are not queued

**Rationale:** Realistic expectation. Transaction includes stop, reconfigure, capture,
encode, restore, and restart - this takes time.

---

## 11. Memory Validation (All Phases)

### ❌ Current Guide Says:
- Uses estimated memory values from plan
- Assumes memory is sufficient

### ✅ Plan V2.2 requires:
- **Runtime measurement before attempting capture**
- Check free PSRAM and largest contiguous block
- Calculate required buffer sizes
- Return 503 Service Unavailable if insufficient memory
- Document measured values, not estimates

**Add to all capture sequences:**
```cpp
bool validate_memory_for_resolution(uint32_t width, uint32_t height) {
  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  
  size_t required_mmap = width * height * 2;  // RGB565
  size_t required_jpeg = width * height / 2;  // Conservative
  size_t total_required = required_mmap + required_jpeg + (1024*1024);  // 1MB margin
  
  if (largest_block < required_mmap || free_psram < total_required) {
    Serial.printf("memory gate: INSUFFICIENT free=%u largest=%u required=%u\n",
                  free_psram, largest_block, total_required);
    return false;
  }
  return true;
}
```

---

## Summary Table

| Topic | Current Guide | Plan V2.2 | Risk if Not Fixed |
|-------|--------------|-----------|-------------------|
| Format priority | YUV420 first | RGB565 first | Encode failure |
| Timeout | Watchdog OK | ESP-Video patch required | Hanging captures |
| Allocator | Not verified | Verification phase required | Memory leaks |
| Controller | ModeManager split | CaptureController unified | Race conditions |
| PhotoStore | Unprotected acquire | Mutex-protected acquire | Use-after-free |
| HTTP framework | AsyncWebServer | esp_http_server | API mismatch |
| API pattern | Synchronous | Retained-photo | Timeout issues |
| Publication | Unclear | Do NOT publish on restore fail | RTSP broken |
| RTSP keepalive | Prescriptive | Measurement-based | Complexity |
| Cadence wording | Matches interval | Interval + transaction | False expectations |
| Memory validation | Estimates | Runtime measurement | OOM crashes |

---

## Recommended Action

**Option 1:** Apply these corrections to current guide systematically  
**Option 2:** Treat current guide as reference only, implement directly from Plan V2.2  
**Option 3:** Create new guide v2.2 from scratch based on Plan V2.2

**Current status:** Execution Guide marked as **DRAFT - NOT ALIGNED WITH PLAN V2.2**
