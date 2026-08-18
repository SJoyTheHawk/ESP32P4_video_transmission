# Phase 7 Resolution Switching Results

**Date:** 2026-08-19  
**Implementation Version:** v3.0-phase7-arduino  
**Status:** Phase 7A-7B Complete (Sensor modes + Controller switching)

## Phase 7A: Sensor Mode Preparation - COMPLETE

### Sensor Modes Implemented

| Resolution | Mode Name | Files Created | Source |
|------------|-----------|---------------|--------|
| 800x800 | XVGA | ov5647_800x800_mode.{h,cpp}, ov5647_800x800_registers.h | Espressif esp_cam_sensor (Apache-2.0) |
| 800x640 | WVGA | ov5647_800x640_mode.{h,cpp}, ov5647_800x640_registers.h | Espressif esp_cam_sensor (Apache-2.0) |

### Source Attribution

All sensor modes imported from:
- **Source:** `reference/17_mipicamera/managed_components/espressif__esp_cam_sensor`
- **Commit:** `7c343dc478f73e3234ed898eb358accd8de92ff7`
- **License:** Apache-2.0
- **Copyright:** 2024 Espressif Systems (Shanghai) CO LTD

Both modes use:
- RAW8 format (8-bit Bayer)
- 2-lane MIPI-CSI
- 24MHz XCLK input
- 50 FPS target
- 100MHz IDI clock rate
- 400MHz MIPI line rate

### VGA (640x480) Status

**Not Available:** The OV5647 sensor does not have a native 640x480 mode in the Espressif component library. Available alternatives from the sensor are:
- 800x640 (WVGA) - implemented
- 800x800 (XVGA) - implemented  
- 800x1280 (portrait) - not implemented
- 1920x1080 (1080p) - already implemented for still capture

Hardware crop via `VIDIOC_S_SELECTION` is not supported in the Arduino ESP-Video build (returns errno=106), so we cannot crop 800x640 to 640x480.

**Recommendation:** Use 800x640 (WVGA) as the primary stream resolution. It's the closest available to VGA and uses less memory than 800x800.

### SVGA (800x600) Status

**Not Available:** The OV5647 does not have a native 800x600 mode. The sensor's native aspect ratios are 4:3 (800x640, 1920x1080), 1:1 (800x800), and tall formats (800x1280).

To achieve 800x600, would require cropping 800x640 by 40 pixels vertically, which requires hardware crop support (unavailable in Arduino build).

## Phase 7B: Controller Switch Logic - COMPLETE

### Implementation

Added to `CaptureController`:
- `switchResolution(StreamResolution target)` - Full switch with rollback
- `getCurrentResolution()` - Query current mode
- `isResolutionSupported()` - Check if resolution available
- `resolutionName()` - Get human-readable name
- `getSensorFormatForResolution()` - Map enum to sensor format
- `memoryGateForResolution()` - Validate memory availability

### Switch Flow

1. Acquire operation mutex (1 second timeout)
2. Validate state is BaselineRunning
3. Check if already at target (early return)
4. Validate target resolution supported
5. Run memory gate for target resolution
6. Save current state for rollback
7. Stop stream, release buffers, end JPEG encoder
8. Apply new sensor format
9. Set RGB565 output format
10. Update dimensions
11. Allocate new buffers
12. Initialize JPEG encoder for new dimensions
13. Start stream
14. Discard settling frames (3 frames)
15. Update current_resolution_
16. Release mutex

On failure at any step, fully rolls back to previous working state.

### Serial Commands Implemented

| Command | Function |
|---------|----------|
| `w` or `W` | Switch to WVGA (800x640) |
| `x` or `X` | Switch to XVGA (800x800) |
| `q` or `Q` | Query current resolution |
| `u` or `U` | Mark photo API unavailable (existing) |
| `r` or `R` | Restore photo API ready (existing) |

### Memory Gates

Resolution switch validates:
- RGB565 buffer size: width × height × 2 bytes
- Total buffers: buffer_size × buffer_count (2)
- JPEG encoder estimate: 300 KB
- Reserve: 512 KB
- Check PSRAM free and largest block

Example for 800x640:
- Buffer size: 800 × 640 × 2 = 1,024,000 bytes (1 MB)
- Total buffers: 2,048,000 bytes (~2 MB)
- Total required: ~2.8 MB

## Phase 7C: Serial Testing - PENDING

### Test Procedure

1. Build and upload firmware
2. Connect serial monitor at 115200 baud
3. Wait for Phase 3-6 gates to pass
4. Send `q` → Should report XVGA (800x800)
5. Send `w` → Switch to WVGA (800x640)
6. Send `q` → Should report WVGA (800x640)
7. Send `x` → Switch back to XVGA (800x800)
8. Repeat 10 times, check for memory leaks

### Expected Serial Output

```
resolution switch: from=XVGA (800x800) to=WVGA (800x640)
resolution switch: success new_resolution=WVGA (800x640)
Resolution switch success: now running at 800x640
```

### Testing with RTSP

1. Start VLC at current resolution
2. Switch resolution via serial
3. Observe VLC behavior (will likely disconnect)
4. Reconnect VLC, verify new resolution

## Phase 7C: HTTP API - COMPLETE

### Endpoints Implemented

**GET /api/stream/resolution**
- Returns current resolution, dimensions, and supported modes
- Response: `{"current":"xvga","width":800,"height":800,"supported":["xvga","wvga"]}`

**PUT /api/stream/resolution**
- Changes stream resolution
- Body: `{"resolution":"xvga"}` or `{"resolution":"wvga"}`
- Returns: `{"status":"success","resolution":"wvga","width":800,"height":640,"previous":"xvga"}`

### Implementation

Added to `PhotoApi`:
- `setCaptureController()` - Associate controller instance
- `handleStreamResolutionGet()` - Query current resolution
- `handleStreamResolutionPut()` - Change resolution
- Simple JSON parsing for request body
- Status codes: 200 (success), 400 (invalid), 500 (switch failed), 503 (unavailable)

### Testing

See `PHASE7C_HTTP_API_TEST.md` for complete test procedures including:
- Basic GET/PUT tests
- Invalid input handling
- RTSP client behavior
- Stress testing (10+ rapid switches)
- Integration with photo capture

### Validation

```bash
# Query current
curl http://192.168.1.91/api/stream/resolution

# Switch to WVGA
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution

# Switch to XVGA
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"xvga"}' \
  http://192.168.1.91/api/stream/resolution
```

## Phase 7D: RTSP Integration Testing - READY

Complete RTSP client testing procedure documented in `PHASE7D_RTSP_INTEGRATION_TEST.md`.

### Test Plan

**Clients to test:**
- VLC Media Player
- ffmpeg (recording/transcoding)
- GStreamer pipeline

**Test scenarios:**
1. Basic streaming at each resolution (XVGA, WVGA, Portrait)
2. Resolution switching during active stream
3. High-resolution photo capture during active stream
4. Stress test: rapid resolution switches
5. Combined: resolution switch + photo capture + verification

### Key Measurements Needed

- Media gap duration during photo capture (from serial log)
- Client reconnection behavior (auto vs manual)
- Stream quality after resolution switch
- Memory stability after multiple switches
- Resolution persistence after photo capture

### Test Commands

```bash
# Start VLC
vlc rtsp://192.168.1.91:554/mjpeg/1

# Switch resolution
curl -X PUT -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution

# Trigger photo capture
curl -X POST -d '{}' http://192.168.1.91/api/photo/capture

# Verify resolution persisted
curl http://192.168.1.91/api/stream/resolution
```

### Expected Behavior

- **Resolution switch:** Most clients disconnect (SDP changed), manual reconnect needed
- **Photo capture:** Brief gap (<2s), automatic resume to current baseline
- **Memory:** Stable across 10+ switches
- **Persistence:** Photo capture restores to current resolution, not hardcoded default

Results will be documented in `phase7d_results.md` after hardware testing.

## Implementation Summary

Will add:
- `GET /api/stream/resolution` - Query current and supported
- `PUT /api/stream/resolution` - Change resolution
- JSON body: `{"resolution": "wvga"}` or `{"resolution": "xvga"}`

## Limitations Documented

1. **No VGA (640x480):** Sensor does not have native 640x480 mode, hardware crop unavailable
2. **No SVGA (800x600):** Sensor does not have native 800x600 mode
3. **Arduino ESP-Video limitation:** `VIDIOC_S_SELECTION` returns ESP_ERR_NOT_SUPPORTED (errno=106)
4. **RTSP clients:** Will likely disconnect during resolution switch, reconnection required
5. **High-res capture:** Must restore to current baseline (not hardcoded 800x800) - already handled by existing code

## Recommendations

1. **Primary stream resolution:** 800x640 (WVGA) - closest to VGA, less memory than 800x800
2. **Fallback resolution:** 800x800 (XVGA) - current baseline, well-tested
3. **VGA future work:** Requires ESP-IDF component build (Phase 1 migration) to patch driver for hardware crop
4. **Default on boot:** Start at 800x640, save preference in Phase 8

## Next Steps

1. Test serial commands on hardware
2. Validate RTSP behavior during switch
3. Implement HTTP API (Phase 7C)
4. Document RTSP client compatibility
5. Consider adding 1280x960 (SXGA) if sensor mode available
