# Phase 10: 5MP Photo Capture Implementation

## Summary
Change the photo API endpoint to capture 5MP (2592x1944) images instead of 1080p (1920x1080) images.

## Files Fixed
All camera mode files have been fixed for the compilation error "assignment of read-only member 'esp_cam_sensor_isp_info_v1_t::version'":

### Fixed Files:
- `ov5647_1080p_mode.cpp` ✅
- `ov5647_1280x720_mode.cpp` ✅
- `ov5647_640x480_mode.cpp` ✅
- `ov5647_5mp_mode.cpp` ✅
- `ov5647_800x640_mode.cpp` ✅
- `ov5647_800x1280_mode.cpp` ✅
- `ov5647_800x800_mode.cpp` ✅

### Fix Applied:
Changed from:
```cpp
static esp_cam_sensor_isp_info_t isp_info = {};
isp_info.isp_v1_info.version = SENSOR_ISP_INFO_VERSION_DEFAULT;
isp_info.isp_v1_info.pclk = 87500000;
// ... etc
```

To:
```cpp
static esp_cam_sensor_isp_info_t isp_info = {
  .isp_v1_info = {
    .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
    .pclk = 87500000,
    .hts = 2844,
    .vts = 1968,
    .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
  }
};
```

## Tasks for 5MP Photo Capture

### 1. Review 5MP Mode Configuration
File: `ov5647_5mp_mode.cpp`

Current 5MP configuration:
- Resolution: 2592x1944
- Format: RAW10
- FPS: 15
- Bayer pattern: BGGR
- MIPI clock: 700000000
- pclk: 87500000

**Action Required:** Check if these values match the reference implementation at:
`/Users/szemy/Downloads/ESP32P4_video_transmission-main/mipi_csi_camera/`

### 2. Add 5MP Support to CaptureController

File: `capture_controller.h` and `capture_controller.cpp`

Current method:
```cpp
bool captureHighResStill(HighResStillCandidate *candidate);
```

**Action Required:**
- Check if `captureHighResStill()` is hardcoded to 1080p
- If yes, make it configurable to support both 1080p and 5MP
- OR create a new method: `bool capture5MPStill(HighResStillCandidate *candidate);`

### 3. Update Photo API Capture Function

File: `mipi_csi_camera.ino` (line 68)

Current code:
```cpp
static bool capturePhotoForApi(void *) {
  HighResStillCandidate candidate;
  if (!capture_controller.captureHighResStill(&candidate)) {
    return false;
  }
  // ... rest of code
}
```

**Action Required:**
- Change to use 5MP capture instead of 1080p
- Option A: Add a parameter to `captureHighResStill()` to specify resolution
- Option B: Call a new `capture5MPStill()` method
- Option C: Make it configurable through settings

### 4. Memory Considerations

5MP images are much larger than 1080p:
- 1080p RGB565: ~4 MB
- 5MP RGB565: ~9.6 MB
- JPEG size will also be significantly larger

**Action Required:**
- Check if PSRAM can handle 5MP capture
- Update memory gates in `capture_controller.cpp`
- May need to increase `kStillJpegCapacity` in main sketch

### 5. Color Profile and Bayer Pattern

Reference implementation location:
`/Users/szemy/Downloads/ESP32P4_video_transmission-main/mipi_csi_camera/`

**Action Required:**
- Compare 5MP mode Bayer pattern (currently BGGR)
- Check if AWB (Auto White Balance) registers are needed
- Verify color accuracy matches reference implementation

### 6. Testing Checklist

After implementation:
- [ ] Code compiles without errors
- [ ] 5MP photo captures successfully via `/api/photo/capture`
- [ ] JPEG file size is reasonable (check memory)
- [ ] Colors look correct (not greenish/washed)
- [ ] RTSP stream still works (doesn't interfere)
- [ ] Memory doesn't leak after multiple captures
- [ ] Settings persistence works with 5MP

## Current Status

✅ Compilation errors fixed
⏳ 5MP capture implementation pending
⏳ Color profile verification pending
⏳ Memory testing pending

## Notes

- The old cropped resolution modes (800x800, 800x640, 800x1280) are no longer used
- Current streaming resolutions: VGA (640x480), HD (1280x720), FHD (1920x1080)
- Photo capture resolution should be independent of streaming resolution
- Consider making photo resolution configurable in web UI settings
