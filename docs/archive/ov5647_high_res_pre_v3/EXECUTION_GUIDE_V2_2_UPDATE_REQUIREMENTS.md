# Execution Guide Update Requirements

**Date:** 2026-08-18  
**For:** OV5647_HIGH_RES_CAPTURE_AI_EXECUTION_GUIDE.md v2.2

## Summary

The Execution Guide needs comprehensive updates to align with Plan V2.2. Rather than making hundreds of small edits, this document lists all required changes systematically.

## Critical Changes Required

### 1. Phase 1: Format Validation
**Current:** Probes YUV420 as primary concern  
**Required:** Probe RGB565 first (required), YUV420 second (optional)

```diff
- bool probeYuv420Support()
+ bool probeFormatSupport()
  
- Expected outcomes: YUV420 found → proceed
+ Expected outcomes: RGB565 found → required to proceed
+                    YUV420 found → optional optimization for later phase
```

### 2. Phase 1.5: Timeout Mechanism (COMPLETE REWRITE)
**Current:** Offers watchdog task as alternative  
**Required:** Two-tier system, ESP-Video patching is Tier 1 (required)

**Remove entirely:**
- All watchdog task code examples
- `CaptureWatchdog` class
- Any mention of watchdog as timeout implementation

**Add instead:**
- Step-by-step ESP-Video component location instructions
- Exact patch location in VFS ioctl handler
- Build integration instructions
- Deterministic fault injection test
- Tier 2 (degraded mode) as fallback only if patching fails

### 3. Phase 1.5: JPEG Allocator Verification (ADD NEW PHASE)
**Current:** Not present  
**Required:** Add complete verification phase before Phase 2

Must include:
1. Locate `jpeg_alloc_encoder_mem()` implementation
2. Identify matching deallocator (`free()` or `jpeg_free_encoder_mem()`)
3. Create `JpegOutputBuffer` adapter class
4. Test adapter with allocate/release cycles
5. Document verified allocator pairing

### 4. Phase 2: Replace ModeManager with CaptureController
**Current:** Creates separate `ModeManager` and `StillCaptureHandler`  
**Required:** Single unified `CaptureController`

**Remove:**
- All `ModeManager` code
- Split between mode management and capture handling
- Direct use of `ESPVideoCaptureDevClass`

**Add:**
- Complete `CaptureController` with state machine
- States: Uninitialized, BaselineRunning, Stopping, SensorConfiguring, BufferAllocating, HighResReady, Capturing, Restoring, Unavailable
- Explicit transition functions for each state change
- Failure handling for each transition
- Publication-on-failure policy implementation

### 5. Phase 3: PhotoStore with Mutex-Protected Acquire
**Current:** Uses `std::shared_ptr` or simple atomic refcounting  
**Required:** Mutex-protected acquire + atomic release

**Critical fix:**
```cpp
// WRONG - current guide (has race condition)
PhotoBlob* blob = store.latest;
blob->acquire();

// RIGHT - required implementation
class PhotoStore {
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

### 6. Phase 4: HTTP Server - Use esp_http_server, Not AsyncWebServer
**Current:** All examples use `AsyncWebServer`  
**Required:** Use `esp_http_server` throughout

**Replace:**
- `AsyncWebServer server(80)` → `httpd_handle_t server`
- `server.on("/path", HTTP_GET, ...)` → `httpd_register_uri_handler()`
- `AsyncWebServerRequest` → `httpd_req_t`
- All async response patterns with synchronous esp_http_server patterns

### 7. Phase 4: Implement Retained-Photo API, Not Synchronous Endpoints
**Current:** `/capture/1080p` returns JPEG immediately  
**Required:** Retained-photo API with metadata polling

**Remove:**
- All synchronous capture-and-return endpoints
- Immediate JPEG response code

**Add:**
- `GET /api/photo/metadata` with state machine (empty, capturing, ready, error, camera_unavailable)
- `GET /api/photo/latest.jpg?id=<photo_id>` with 410 Gone for old IDs
- `POST /api/photo/capture` returns 202 Accepted immediately
- PhotoStore integration with acquire/release
- ETag and photo_id semantics
- 503 Service Unavailable when camera is Unavailable

### 8. Phase 5: RTSP Measurement-Based Approach
**Current:** Prescribes RTCP sender reports  
**Required:** Measure first, add keepalive only if needed

**Change:**
```diff
- To prevent RTSP client timeout:
-   - Send RTCP sender reports every 5 seconds
-   - OR implement RTSP OPTIONS keepalive

+ Measurement requirements:
+   1. Implement on-demand capture without keepalive code
+   2. Test with VLC, ffmpeg, GStreamer
+   3. Document measured media gap duration
+   4. ONLY add keepalive if measurement shows it's necessary
```

### 9. Phase 6: Background Cadence Wording
**Current:** "Actual cadence matches configured interval"  
**Required:** "Actual cadence = interval + transaction duration"

**Fix all mentions of:**
- Cadence expectations
- Performance metrics
- Test acceptance criteria

### 10. Remove Out-of-Scope Features
**Current:** Includes gallery UI, quality parameters, reset endpoints  
**Required:** Mark as optional post-core functionality

**Move to separate "Phase 7: Optional Enhancements" section:**
- Landing page with gallery
- Runtime quality parameter support
- Diagnostics endpoint
- Any unauthenticated reset/force-mode endpoints (remove entirely)

## Memory Validation Addition

Add to all capture phases:

```cpp
struct MemoryReport validate_resolution_memory(uint32_t width, uint32_t height) {
  // Runtime measurement before attempting capture
  // Check free PSRAM, largest block
  // Calculate required sizes
  // Return has_required_reserve boolean
}

// Use before enabling any resolution
if (!validate_resolution_memory(1920, 1080).has_required_reserve) {
  return 503;  // Service Unavailable
}
```

## State Machine Documentation

Every phase that modifies controller state must include:

1. Current state
2. Operation performed
3. Success next state
4. Failure next state
5. Cleanup on failure
6. What happens to in-progress photo

## Code Example Safety

Every code example must include:

```cpp
// ⚠️ ILLUSTRATIVE CODE - Validate against installed APIs
// This example must be adapted for:
// - Installed ESP-Video version
// - esp_cam_sensor structure fields
// - JPEG encoder API surface
// - HTTP server framework version
```

## Testing Requirements

Add to each phase:

1. **Compilation test:** Code must compile
2. **Unit test:** Isolated component test where possible
3. **Integration test:** With other components
4. **Failure test:** Deliberate failure injection
5. **Memory test:** Allocation/deallocation verification
6. **State test:** Controller enters expected states

## Documentation Requirements

Each phase result document must include:

1. **Implementation status:** Complete/Incomplete with rationale
2. **Test results:** Pass/Fail with actual output
3. **Performance metrics:** Actual measured values (not estimates)
4. **Issues encountered:** List with resolutions
5. **Decisions made:** With technical rationale
6. **Ready for next phase:** Yes/No with blocking issues listed

## Final Structure

Recommended phase order after updates:

```
Phase 0: Hardware Baseline Validation
Phase 1: Format Support Validation (RGB565 required, YUV420 optional)
Phase 1.5a: ESP-Video Timeout Patching (Tier 1 - required)
Phase 1.5b: JPEG Allocator Verification
Phase 2: CaptureController with State Machine (RGB565 1080p)
Phase 3: PhotoStore with Mutex-Protected Acquire
Phase 4: esp_http_server with Retained-Photo API
Phase 5: RTSP Measurement and Integration
Phase 6: Background Scheduling
Phase 7: Optional Enhancements (gallery, quality params)
Phase 8: Optional YUV420 Optimization
Phase 9: Optional 5MP Support
Phase 10: Final Testing and Documentation
```

## Implementation Status

- [x] Plan V2.2 updated with all corrections
- [ ] Execution Guide Phase 0 (OK as-is)
- [ ] Execution Guide Phase 1 (needs RGB565-first emphasis)
- [ ] Execution Guide Phase 1.5 (needs complete rewrite)
- [ ] Execution Guide Phase 2 (needs CaptureController replacement)
- [ ] Execution Guide Phase 3 (needs PhotoStore mutex fix)
- [ ] Execution Guide Phase 4 (needs esp_http_server + retained-photo API)
- [ ] Execution Guide Phase 5 (needs measurement approach)
- [ ] Execution Guide Phase 6 (needs cadence wording fix)
- [ ] Execution Guide Phase 7+ (needs reorganization)

## Recommendation

Given the extent of required changes, I recommend:

**Option A:** Complete rewrite of Phases 1-6 based on Plan V2.2  
**Option B:** Create a separate "Execution Guide V2.2 Addendum" with corrections  
**Option C:** Mark current guide as "Draft - Requires V2.2 Alignment" and create new guide

The current guide has valuable phase structure and testing ideas, but the code examples
need systematic replacement to match Plan V2.2 architecture.
