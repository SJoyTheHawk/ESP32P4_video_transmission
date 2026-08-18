# Dynamic Resolution Switching Implementation Plan

**Version:** 1.0  
**Date:** 2026-08-19  
**Target:** ESP32-P4 Arduino build with runtime stream resolution control  
**Primary Goals:** SVGA (800x600) and VGA (640x480)

## 1. Overview

Add the ability to dynamically switch the RTSP/RTP-JPEG stream resolution at runtime without rebooting the device. This requires stopping the current stream, reconfiguring the sensor and V4L2 pipeline, and restarting with the new resolution.

## 2. Target Resolutions

| Priority | Name | Resolution | Notes |
|----------|------|------------|-------|
| Primary | SVGA | 800x600 | OV5647 native mode likely available |
| Primary | VGA | 640x480 | May require 800x640 sensor + crop |
| Fallback | XVGA | 800x800 | Current baseline, proven working |
| Bonus | SXGA | 1280x960 | If sensor mode available |
| Bonus | 720p | 1280x720 | If sensor mode available |

## 3. Architecture Approach

### 3.1 Sensor Mode Discovery

The OV5647 sensor supports multiple resolutions through different register configurations. We need:

1. **Direct sensor modes** - Complete register tables that set the sensor to output the exact target resolution
2. **Sensor + crop modes** - Larger sensor output with V4L2 hardware crop to target (if `VIDIOC_S_SELECTION` works)
3. **Sensor + V4L2 format modes** - Sensor outputs larger, V4L2 driver scales/crops to match requested format

### 3.2 Resolution Switching Flow

```
Current State: BaselineRunning @ 800x800
         |
         v
Receive switch command (e.g., "switch to VGA")
         |
         v
Validate target resolution is supported
         |
         v
Stop RTSP stream (RTCP BYE if needed)
         |
         v
CaptureController::switchResolution(target)
    - Stop V4L2 stream (VIDIOC_STREAMOFF)
    - Release buffers
    - Apply new sensor format
    - Set new V4L2 format
    - Request/map new buffers
    - Start V4L2 stream (VIDIOC_STREAMON)
    - Update baseline_sensor_format_
         |
         v
Restart RTSP with new dimensions
         |
         v
New State: BaselineRunning @ new resolution
```

### 3.3 Key Constraints

- **Single pipeline**: Camera can only output one resolution at a time
- **RTSP clients**: Most clients will disconnect during the switch; reconnection is expected
- **High-res capture**: Must save/restore the new baseline, not always 800x800
- **Memory**: Each resolution has different buffer sizes; validate before switching
- **Atomicity**: Either fully switch or restore the previous working resolution

## 4. Implementation Components

### 4.1 Sensor Mode Descriptors

Create sensor mode files for each target resolution:

```
mipi_csi_camera/
  ov5647_800x800_mode.h       # Current baseline (already implicit)
  ov5647_800x600_mode.h       # New: SVGA
  ov5647_800x600_mode.cpp
  ov5647_800x600_registers.h
  ov5647_640x480_mode.h       # New: VGA (if direct mode exists)
  ov5647_640x480_mode.cpp
  ov5647_640x480_registers.h
```

Each mode needs a complete `esp_cam_sensor_format_t` descriptor with:
- Width, height, FPS
- Bayer order, bit width
- MIPI lane configuration
- Complete register tables for: reset, PLL, sensor crop, output format, timing, exposure

**Source**: Import from Espressif `esp-video-components` or compatible OV5647 drivers, document the source commit and license.

### 4.2 CaptureController Extensions

Add to `capture_controller.h`:

```cpp
enum class StreamResolution : uint8_t {
  XVGA_800x800,
  SVGA_800x600,
  VGA_640x480,
};

class CaptureController {
public:
  // New methods
  bool switchResolution(StreamResolution target);
  StreamResolution getCurrentResolution() const;
  bool isResolutionSupported(StreamResolution resolution) const;
  
private:
  StreamResolution current_resolution_ = StreamResolution::XVGA_800x800;
  const esp_cam_sensor_format_t* getSensorFormatForResolution(
    StreamResolution resolution) const;
};
```

Add to `capture_controller.cpp`:

```cpp
bool CaptureController::switchResolution(StreamResolution target) {
  // Acquire operation lock
  if (xSemaphoreTake(operation_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  
  if (state_ != CaptureControllerState::BaselineRunning) {
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Save current state for rollback
  StreamResolution previous_resolution = current_resolution_;
  esp_cam_sensor_format_t previous_format = baseline_sensor_format_;
  
  // Get target sensor format
  const esp_cam_sensor_format_t* target_format = 
    getSensorFormatForResolution(target);
  if (target_format == nullptr) {
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Memory gate: check if new resolution fits
  const size_t required_buffer_bytes = 
    target_format->width * target_format->height * 2; // RGB565
  const size_t total_required = required_buffer_bytes * requested_buffer_count_;
  // Add JPEG encoder memory, etc.
  
  if (!memoryGateForResolution(total_required)) {
    Serial.printf("resolution switch blocked: insufficient memory\n");
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Stop current stream
  stopStream();
  releaseBuffers();
  jpeg_encoder_.end();
  
  // Apply new sensor format
  setState(CaptureControllerState::SensorConfiguring);
  if (!applySensorFormat(*target_format, true)) {
    // Rollback
    applySensorFormat(previous_format, true);
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_format.width, previous_format.height, 
                        jpeg_quality_);
    startStream();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Set RGB565 output
  if (!setRgb565Output()) {
    // Rollback
    applySensorFormat(previous_format, true);
    setRgb565Output();
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_format.width, previous_format.height,
                        jpeg_quality_);
    startStream();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Update dimensions
  width_ = target_format->width;
  height_ = target_format->height;
  baseline_sensor_format_ = *target_format;
  
  // Allocate new buffers
  if (!requestAndMapBuffers(requested_buffer_count_)) {
    // Rollback
    applySensorFormat(previous_format, true);
    setRgb565Output();
    width_ = previous_format.width;
    height_ = previous_format.height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_format.width, previous_format.height,
                        jpeg_quality_);
    startStream();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Reinitialize JPEG encoder for new dimensions
  if (!jpeg_encoder_.begin(width_, height_, jpeg_quality_)) {
    // Rollback
    releaseBuffers();
    applySensorFormat(previous_format, true);
    setRgb565Output();
    width_ = previous_format.width;
    height_ = previous_format.height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_format.width, previous_format.height,
                        jpeg_quality_);
    startStream();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Start new stream
  if (!startStream()) {
    // Rollback
    jpeg_encoder_.end();
    releaseBuffers();
    applySensorFormat(previous_format, true);
    setRgb565Output();
    width_ = previous_format.width;
    height_ = previous_format.height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_format.width, previous_format.height,
                        jpeg_quality_);
    startStream();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  
  // Discard settling frames
  discardSettlingFrames(kBaselineSettlingFrames);
  
  // Success
  current_resolution_ = target;
  setState(CaptureControllerState::BaselineRunning);
  
  Serial.printf(
    "resolution switch: status=success from=%dx%d to=%dx%d\n",
    previous_format.width, previous_format.height,
    width_, height_);
  
  xSemaphoreGive(operation_mutex_);
  return true;
}
```

### 4.3 HTTP API Extension

Add new endpoint to `photo_api.cpp`:

```cpp
// PUT /api/stream/resolution
// Body: {"resolution": "svga"} or {"resolution": "vga"}

static esp_err_t handleStreamResolutionPut(httpd_req_t *req) {
  // Parse JSON body
  // Map string to StreamResolution enum
  // Call capture_controller.switchResolution(target)
  // Return 200 on success, 400 on invalid, 503 on failure
}
```

Response format:

```json
{
  "status": "success",
  "resolution": "svga",
  "width": 800,
  "height": 600,
  "previous_resolution": "xvga"
}
```

Also add GET endpoint:

```cpp
// GET /api/stream/resolution
// Returns current resolution and supported list

{
  "current": "xvga",
  "width": 800,
  "height": 600,
  "supported": ["xvga", "svga", "vga"]
}
```

### 4.4 Serial Command Interface (Testing)

For testing without a web panel, add serial commands in `loop()`:

```cpp
if (Serial.available()) {
  char cmd = Serial.read();
  switch (cmd) {
    case 'v':  // Switch to VGA
      if (capture_controller.switchResolution(StreamResolution::VGA_640x480)) {
        Serial.println("Switched to VGA (640x480)");
      }
      break;
    case 's':  // Switch to SVGA
      if (capture_controller.switchResolution(StreamResolution::SVGA_800x600)) {
        Serial.println("Switched to SVGA (800x600)");
      }
      break;
    case 'x':  // Switch to XVGA
      if (capture_controller.switchResolution(StreamResolution::XVGA_800x800)) {
        Serial.println("Switched to XVGA (800x800)");
      }
      break;
    case 'q':  // Query current resolution
      StreamResolution current = capture_controller.getCurrentResolution();
      Serial.printf("Current: %s (%ux%u)\n", 
                    resolutionName(current),
                    capture_controller.getWidth(),
                    capture_controller.getHeight());
      break;
  }
}
```

### 4.5 RTSP Integration

The RTSP server in `loop()` needs to handle resolution changes:

```cpp
void loop() {
  #ifndef EXCLUDE_WIFI
  // Check if resolution changed
  static uint32_t last_width = 0;
  static uint32_t last_height = 0;
  
  if (last_width != capture_controller.getWidth() ||
      last_height != capture_controller.getHeight()) {
    // Resolution changed, log it
    Serial.printf("RTSP: stream dimensions changed to %ux%u\n",
                  capture_controller.getWidth(),
                  capture_controller.getHeight());
    last_width = capture_controller.getWidth();
    last_height = capture_controller.getHeight();
    // Note: Most RTSP clients will need to reconnect
  }
  
  // Existing frame acquisition and sending
  BaselineFrame frame = capture_controller.acquireBaselineFrame();
  if (frame.valid()) {
    // Send with current dimensions
    rtspServer.streamFrame(frame.data(), frame.size());
    frame.end();
  }
  #endif
}
```

## 5. Implementation Phases

### Phase 7A: Sensor Mode Preparation

1. Research and import OV5647 sensor modes for 800x600 and 640x480
2. Create descriptor files with complete register tables
3. Document source, commit hash, and license
4. Add unit tests that apply each mode individually

**Exit gate:** All sensor descriptors compile and can be applied via `VIDIOC_S_SENSOR_FMT` individually

### Phase 7B: Controller Switch Logic

1. Implement `switchResolution()` with full rollback
2. Add memory gate validation
3. Implement serial test commands
4. Test switching between all supported modes

**Exit gate:** Serial commands can switch between modes, V4L2 readback confirms dimensions

### Phase 7C: HTTP API

1. Add `/api/stream/resolution` GET and PUT endpoints
2. Implement JSON parsing and validation
3. Test with curl commands

**Exit gate:** HTTP API can query and change resolution, invalid requests return 400

### Phase 7D: RTSP Integration Testing

1. Test resolution switching with active RTSP clients
2. Measure reconnection behavior
3. Document client compatibility

**Exit gate:** VLC/ffmpeg can reconnect after resolution switch, new dimensions are correct

## 6. Constraints and Limitations

### 6.1 Hardware Crop Availability

Previous testing showed `VIDIOC_S_SELECTION` returns `ESP_ERR_NOT_SUPPORTED` (errno=106) in the Arduino ESP-Video build. This means:

- **Direct sensor modes only**: Each target resolution needs a complete sensor mode
- **No crop-based VGA**: Cannot use 800x640 sensor + 640x480 crop
- **Must find native modes**: Need to locate OV5647 register tables for exact target resolutions

If a direct 640x480 sensor mode doesn't exist, alternatives:
1. Use 640x512 or closest available mode
2. Document VGA as unsupported until ESP-IDF component build (Phase 1 migration)
3. Accept 800x600 as the primary target

### 6.2 High-Resolution Capture Impact

After resolution switching, high-resolution capture must:
- Save the NEW baseline format, not hardcoded 800x800
- Restore to the CURRENT baseline after 1080p capture
- This already works if `baseline_sensor_format_` is properly maintained

### 6.3 RTSP Client Behavior

Most RTSP clients will:
- Disconnect when stream stops
- Need manual reconnection to see new resolution
- Some clients may cache SDP and refuse to reconnect with different dimensions

The switching feature is primarily for setup/configuration, not continuous dynamic adaptation.

### 6.4 Memory Requirements

Different resolutions have different buffer sizes:

| Resolution | RGB565 Buffer | 2 Buffers | JPEG Encoder | Total |
|------------|--------------|-----------|--------------|-------|
| 800x800 | 1.28 MB | 2.56 MB | ~300 KB | ~2.9 MB |
| 800x600 | 0.96 MB | 1.92 MB | ~300 KB | ~2.2 MB |
| 640x480 | 0.61 MB | 1.22 MB | ~300 KB | ~1.5 MB |

Smaller resolutions use less memory, which improves stability and headroom for high-resolution capture.

## 7. Testing Plan

### 7.1 Unit Tests (Serial Interface)

```bash
# Connect to serial monitor
# Send 'x' -> Verify 800x800
# Send 's' -> Verify 800x600
# Send 'v' -> Verify 640x480
# Send 'x' -> Verify 800x800 again
# Repeat 10 times, check for memory leaks
```

### 7.2 HTTP API Tests

```bash
# Query current
curl http://192.168.1.91/api/stream/resolution

# Switch to SVGA
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"svga"}' \
  http://192.168.1.91/api/stream/resolution

# Switch to VGA
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"vga"}' \
  http://192.168.1.91/api/stream/resolution

# Invalid request
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"invalid"}' \
  http://192.168.1.91/api/stream/resolution
# Expect 400
```

### 7.3 RTSP Client Tests

```bash
# Start VLC at 800x800
vlc rtsp://192.168.1.91:554/mjpeg/1

# Switch to 800x600 via HTTP
curl -X PUT -d '{"resolution":"svga"}' ...

# Reconnect VLC, verify 800x600 stream
# Repeat for 640x480
```

### 7.4 High-Res Capture After Switch

```bash
# Switch to SVGA
curl -X PUT -d '{"resolution":"svga"}' ...

# Trigger 1080p capture
curl -X POST -d '{}' http://192.168.1.91/api/photo/capture

# Verify RTSP stream returns to SVGA (not XVGA)
# Check serial log confirms baseline restored to 800x600
```

## 8. Rollout Strategy

### Option A: Incremental (Recommended)

1. **Phase 7A**: Add sensor modes, test individually with `applySensorFormat()`
2. **Phase 7B**: Add `switchResolution()` with serial commands only
3. **Phase 7C**: Add HTTP API after serial testing passes
4. **Phase 7D**: Test RTSP integration and document behavior

This allows testing each piece before exposing it via HTTP.

### Option B: Web Panel First

If you want to build the web panel before implementing switching:
1. Design the web UI with resolution dropdown
2. Implement the backend API as placeholder (returns 501 Not Implemented)
3. Implement Phase 7A-D
4. Enable the API once backend is ready

## 9. Success Criteria

Resolution switching is complete when:

- SVGA (800x600) sensor mode implemented and tested
- VGA (640x480) sensor mode implemented (or documented as unavailable)
- `switchResolution()` works with full rollback on failure
- HTTP API can query and change resolution
- Serial test commands work for manual testing
- RTSP stream reflects new resolution after switch
- High-resolution capture correctly restores to current baseline
- 20 consecutive switches show no memory leak
- All resolution modes documented with source attribution

## 10. Future Enhancements

After basic switching works:

- **Persistent setting**: Save resolution in `Preferences`, apply on boot
- **Auto-selection**: Adjust resolution based on available PSRAM
- **Quality per resolution**: Different JPEG quality for different resolutions
- **Bitrate hints**: Provide bandwidth estimates in API response
- **Web UI**: Dropdown selector with live preview dimensions
- **720p/1080p streaming**: If memory allows (requires more buffers)
