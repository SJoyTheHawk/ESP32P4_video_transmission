# Phase 8: Settings Persistence and Background Scheduling

**Version:** 1.0  
**Date:** 2026-08-19  
**Prerequisites:** Phase 7 complete (resolution switching working)

## Overview

Add persistent settings storage and optional background capture scheduling to the ESP32-P4 camera system. Settings should survive reboots, and the system should optionally support scheduled high-resolution captures.

## Goals

1. **Settings Persistence**: Save stream resolution, JPEG quality, and other configuration to NVS
2. **Auto-restore on Boot**: Apply saved settings during startup
3. **HTTP API for Settings**: GET/PUT endpoints for configuration management
4. **Background Scheduling** (Optional): Periodic high-resolution captures with configurable intervals

## Implementation Components

### 8.1 Settings Structure

Create `settings.h`:

```cpp
struct CameraSettings {
  StreamResolution stream_resolution;
  uint8_t jpeg_quality;
  bool auto_start_stream;
  
  // Background capture settings (optional)
  bool enable_background_capture;
  uint32_t capture_interval_seconds;
  
  // Validation
  bool isValid() const;
  void setDefaults();
};
```

### 8.2 Settings Manager

Create `settings_manager.h` and `settings_manager.cpp`:

```cpp
class SettingsManager {
public:
  bool begin(const char* nvs_namespace = "camera");
  
  bool loadSettings(CameraSettings& settings);
  bool saveSettings(const CameraSettings& settings);
  bool resetToDefaults();
  
private:
  Preferences prefs_;
};
```

Use Arduino `Preferences` library (wraps ESP-IDF NVS):
- Save as binary blob or individual keys
- Validate on load, use defaults if corrupted

### 8.3 Integration Points

**In `mipi_csi_camera.ino` setup():**

```cpp
SettingsManager settings_manager;
CameraSettings settings;

void setup() {
  // ... existing init ...
  
  // Load settings
  settings_manager.begin();
  if (!settings_manager.loadSettings(settings)) {
    Serial.println("settings: using defaults");
    settings.setDefaults();
  }
  
  // Apply stream resolution
  if (settings.stream_resolution != StreamResolution::XVGA_800x800) {
    if (capture_controller.switchResolution(settings.stream_resolution)) {
      Serial.printf("settings: restored resolution to %s\n",
                    capture_controller.resolutionName(settings.stream_resolution));
    }
  }
  
  // Apply JPEG quality if changed
  // capture_controller.setJpegQuality(settings.jpeg_quality);
}
```

**In `photo_api.cpp`:**

Add endpoints:
- `GET /api/settings` - Return all settings as JSON
- `PUT /api/settings` - Update settings and persist
- `POST /api/settings/reset` - Reset to defaults

### 8.4 HTTP API Examples

**GET /api/settings:**
```json
{
  "stream_resolution": "wvga",
  "jpeg_quality": 80,
  "auto_start_stream": true,
  "background_capture": {
    "enabled": false,
    "interval_seconds": 300
  }
}
```

**PUT /api/settings:**
```json
{
  "stream_resolution": "portrait",
  "jpeg_quality": 85
}
```

Response:
```json
{
  "status": "success",
  "applied": {
    "stream_resolution": "portrait",
    "jpeg_quality": 85
  },
  "requires_reboot": false
}
```

### 8.5 Background Scheduling (Optional)

If `enable_background_capture` is true:

```cpp
void loop() {
  static unsigned long last_capture_ms = 0;
  
  if (settings.enable_background_capture) {
    unsigned long now = millis();
    if (now - last_capture_ms >= settings.capture_interval_seconds * 1000) {
      // Trigger background capture
      photo_api.capturePhotoAsync();
      last_capture_ms = now;
    }
  }
  
  // ... existing RTSP loop ...
}
```

## Implementation Phases

### Phase 8A: Settings Structure and Manager
- Create `settings.h` with `CameraSettings` struct
- Create `settings_manager.h/.cpp` with NVS read/write
- Add defaults: XVGA, quality=80, auto_start=true
- Test save/load with serial commands

**Exit gate:** Settings can be saved and restored across reboots

### Phase 8B: Boot-time Restoration
- Integrate settings load in `setup()`
- Apply saved resolution via `switchResolution()`
- Test with different saved resolutions

**Exit gate:** Device boots to saved resolution, serial log confirms restoration

### Phase 8C: HTTP API for Settings
- Add `GET /api/settings` endpoint
- Add `PUT /api/settings` endpoint
- Add `POST /api/settings/reset` endpoint
- Test with curl

**Exit gate:** Settings can be queried and changed via HTTP, persist across reboots

### Phase 8D: Background Scheduling (Optional)
- Add scheduling logic to `loop()`
- Add enable/disable via settings API
- Test with short intervals (30s) to verify timing

**Exit gate:** Background captures trigger on schedule when enabled

## Testing Plan

### Test 8A: Settings Persistence
```bash
# Set resolution to Portrait via HTTP
curl -X PUT -d '{"stream_resolution":"portrait"}' http://192.168.1.91/api/settings

# Reboot device
# Serial monitor should show: "settings: restored resolution to Portrait"
# RTSP stream should be 800x1280

# Query settings
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "portrait"
```

### Test 8B: Settings Reset
```bash
# Reset to defaults
curl -X POST http://192.168.1.91/api/settings/reset

# Query settings
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "xvga"

# Reboot
# Should start at XVGA
```

### Test 8C: Background Scheduling (Optional)
```bash
# Enable background capture every 60 seconds
curl -X PUT -d '{"background_capture":{"enabled":true,"interval_seconds":60}}' \
  http://192.168.1.91/api/settings

# Watch serial log for automatic captures
# Should see capture triggers at 60-second intervals
```

## Validation Gates

Phase 8 complete when:
- ✅ Settings persist across reboots in NVS
- ✅ Boot-time resolution restoration works
- ✅ HTTP API can read/write settings
- ✅ Settings reset command works
- ✅ Invalid settings rejected with 400 error
- ✅ (Optional) Background scheduling triggers captures on interval

## Notes

- Keep settings structure small (< 512 bytes for NVS efficiency)
- Validate all settings on load to prevent boot loops from corrupt data
- Background scheduling is optional - can be deferred to later phase
- Consider adding OTA update settings in future phases
