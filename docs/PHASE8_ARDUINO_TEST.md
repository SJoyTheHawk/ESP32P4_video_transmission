# Phase 8 Arduino Testing: Settings Persistence

**Date:** 2026-08-19  
**Implementation:** v3.0-phase8-arduino  
**Prerequisites:** Phase 7 complete (resolution switching working)

## Test Objectives

1. Verify settings persist across reboots in NVS
2. Verify boot-time resolution restoration works
3. Verify HTTP settings API (GET/PUT/reset)
4. Verify invalid settings are rejected

## Test 8A: Settings Persistence

### Setup
- Flash Phase 8 firmware
- Device should boot to default XVGA (800x800)

### Test Steps

```bash
# 1. Query default settings
curl http://192.168.1.91/api/settings

# Expected response:
# {
#   "stream_resolution": "xvga",
#   "jpeg_quality": 80,
#   "auto_start_stream": true,
#   "background_capture": {
#     "enabled": false,
#     "interval_seconds": 300
#   }
# }

# 2. Change resolution to Portrait via HTTP
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"stream_resolution":"portrait"}' \
  http://192.168.1.91/api/settings

# Expected response:
# {
#   "status": "success",
#   "applied": {
#     "stream_resolution": "portrait",
#     "jpeg_quality": 80
#   },
#   "requires_reboot": false
# }

# 3. Verify settings saved
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "portrait"

# 4. Reboot device (power cycle or EN button)

# 5. Wait for boot, check serial log
# Should see: "settings: restored resolution to Portrait (800x1280)"

# 6. Query settings after reboot
curl http://192.168.1.91/api/settings
# Should still show "stream_resolution": "portrait"

# 7. Check RTSP stream dimensions
ffmpeg -i rtsp://192.168.1.91:554/mjpeg/1 -frames:v 1 test.jpg
# Should show 800x1280
```

**Pass Criteria:**
- ✅ Settings API returns current configuration
- ✅ PUT changes resolution and saves to NVS
- ✅ After reboot, device starts with saved resolution
- ✅ Serial log confirms restoration
- ✅ RTSP stream matches saved resolution

## Test 8B: Settings Reset

```bash
# 1. Set resolution to WVGA
curl -X PUT -d '{"stream_resolution":"wvga"}' \
  http://192.168.1.91/api/settings

# 2. Verify changed
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "wvga"

# 3. Reset to defaults
curl -X POST http://192.168.1.91/api/settings/reset

# Expected response:
# {
#   "status": "success",
#   "message": "settings reset to defaults"
# }

# 4. Query settings
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "xvga"

# 5. Reboot device

# 6. After boot, query settings
curl http://192.168.1.91/api/settings
# Should show "stream_resolution": "xvga"
```

**Pass Criteria:**
- ✅ Reset command clears saved settings
- ✅ Device returns to XVGA default
- ✅ After reboot, still at default

## Test 8C: Invalid Settings Rejection

```bash
# 1. Invalid resolution name
curl -X PUT -d '{"stream_resolution":"invalid"}' \
  http://192.168.1.91/api/settings
# Expected: 400 Bad Request

# 2. Invalid quality (too low)
curl -X PUT -d '{"jpeg_quality":5}' \
  http://192.168.1.91/api/settings
# Expected: 400 Bad Request

# 3. Invalid quality (too high)
curl -X PUT -d '{"jpeg_quality":150}' \
  http://192.168.1.91/api/settings
# Expected: 400 Bad Request

# 4. Valid quality change
curl -X PUT -d '{"jpeg_quality":85}' \
  http://192.168.1.91/api/settings
# Expected: 200 OK with updated quality
```

**Pass Criteria:**
- ✅ Invalid resolution names rejected with 400
- ✅ Out-of-range quality values rejected with 400
- ✅ Valid quality values accepted
- ✅ Device doesn't crash on invalid input

## Test 8D: Multiple Setting Changes

```bash
# Change both resolution and quality in one request
curl -X PUT -d '{"stream_resolution":"wvga","jpeg_quality":90}' \
  http://192.168.1.91/api/settings

# Expected response:
# {
#   "status": "success",
#   "applied": {
#     "stream_resolution": "wvga",
#     "jpeg_quality": 90
#   },
#   "requires_reboot": false
# }

# Verify both applied
curl http://192.168.1.91/api/settings
# Should show both updated values

# Reboot and verify persistence
```

**Pass Criteria:**
- ✅ Multiple settings can be changed in one request
- ✅ All changes persisted to NVS
- ✅ All changes restored after reboot

## Serial Commands

Phase 8 retains Phase 7 serial commands:
- `w` or `W` - Switch to WVGA (800x640)
- `x` or `X` - Switch to XVGA (800x800)
- `p` or `P` - Switch to Portrait (800x1280)
- `q` or `Q` - Query current resolution

Note: Serial commands do NOT save to NVS. Only HTTP API changes persist.

## Expected Serial Output

**On first boot (no saved settings):**
```
settings: no saved settings found, using defaults
```

**On boot with saved settings:**
```
settings: loaded from NVS resolution=portrait quality=80
settings: restored resolution to Portrait (800x1280)
```

**After settings change via HTTP:**
```
resolution switch: status=success from=800x800 to=800x1280
```

## Phase 8 Validation Gates

Phase 8 complete when:
- ✅ Settings persist across reboots in NVS
- ✅ Boot-time resolution restoration works
- ✅ HTTP API can read/write settings
- ✅ Settings reset command works
- ✅ Invalid settings rejected with 400 error
- ✅ Device never boots into corrupt state

## Known Limitations

1. **JPEG quality change**: Currently saves to settings but doesn't apply to encoder (would require controller method)
2. **Background capture**: Settings structure includes it but not implemented in loop() yet (optional for Phase 8)
3. **Serial commands**: Don't persist to NVS, only HTTP API changes are saved

## Troubleshooting

**Device boots to default despite saved settings:**
- Check serial log for "settings: loaded from NVS"
- If missing, NVS may be corrupted - use reset API
- Verify settings structure magic number matches

**Resolution switch fails on boot:**
- Check PSRAM availability in serial log
- Verify sensor mode files are included in build
- Device will stay at default if switch fails

**Settings API returns 503:**
- Verify `photo_api.setSettingsManager()` called in setup()
- Check settings_manager initialized before photo_api
