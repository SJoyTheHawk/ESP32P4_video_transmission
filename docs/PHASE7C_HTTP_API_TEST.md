# Phase 7C HTTP API Testing Guide

**Purpose:** Test resolution switching via HTTP endpoints

## API Endpoints

### GET /api/stream/resolution

Query current resolution and supported modes.

```bash
curl http://192.168.1.91/api/stream/resolution
```

**Response:**
```json
{
  "current": "xvga",
  "width": 800,
  "height": 800,
  "supported": ["xvga", "wvga"]
}
```

### PUT /api/stream/resolution

Change the stream resolution.

```bash
# Switch to WVGA (800x640)
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution

# Switch to XVGA (800x800)
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"xvga"}' \
  http://192.168.1.91/api/stream/resolution
```

**Success Response (200):**
```json
{
  "status": "success",
  "resolution": "wvga",
  "width": 800,
  "height": 640,
  "previous": "xvga"
}
```

**Error Responses:**

| Status | Condition |
|--------|-----------|
| 400 Bad Request | Invalid JSON or unsupported resolution |
| 500 Internal Server Error | Switch failed (rollback occurred) |
| 503 Service Unavailable | Controller not available or not in baseline state |

## Test Sequence

### 1. Query Current Resolution

```bash
curl -i http://192.168.1.91/api/stream/resolution
```

Expected: 200 OK with current=xvga, width=800, height=800

### 2. Switch to WVGA

```bash
curl -i -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution
```

Expected: 200 OK with status=success, width=800, height=640

Serial log should show:
```
resolution switch: from=XVGA (800x800) to=WVGA (800x640)
resolution switch: success new_resolution=WVGA (800x640)
```

### 3. Verify Resolution Changed

```bash
curl http://192.168.1.91/api/stream/resolution
```

Expected: current=wvga, width=800, height=640

### 4. Switch Back to XVGA

```bash
curl -i -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"xvga"}' \
  http://192.168.1.91/api/stream/resolution
```

Expected: 200 OK with width=800, height=800

### 5. Test Invalid Resolution

```bash
curl -i -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"invalid"}' \
  http://192.168.1.91/api/stream/resolution
```

Expected: 400 Bad Request with error message

### 6. Test Missing Field

```bash
curl -i -X PUT -H 'Content-Type: application/json' \
  -d '{"wrong":"field"}' \
  http://192.168.1.91/api/stream/resolution
```

Expected: 400 Bad Request

### 7. Test During High-Res Capture

```bash
# Start a photo capture
curl -X POST -H 'Content-Type: application/json' \
  -d '{}' http://192.168.1.91/api/photo/capture

# Immediately try to switch resolution
curl -i -X PUT -H 'Content-Type: application/json' \
  -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution
```

Expected: 503 Service Unavailable (controller busy) or timeout waiting for mutex

### 8. RTSP Client Test

```bash
# Start VLC
vlc rtsp://192.168.1.91:554/mjpeg/1

# While streaming, switch resolution
curl -X PUT -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution

# Observe VLC behavior - will likely disconnect
# Reconnect VLC and verify new resolution (800x640)
```

### 9. Stress Test

```bash
# Rapid switching (10 times)
for i in {1..10}; do
  curl -X PUT -d '{"resolution":"wvga"}' http://192.168.1.91/api/stream/resolution
  sleep 1
  curl -X PUT -d '{"resolution":"xvga"}' http://192.168.1.91/api/stream/resolution
  sleep 1
done

# Check for memory leaks in serial log
```

## Success Criteria

- [x] GET returns current resolution and dimensions correctly
- [x] PUT with "xvga" switches to 800x800
- [x] PUT with "wvga" switches to 800x640
- [x] Invalid resolution returns 400
- [x] Resolution persists after switch (verified with GET)
- [x] Serial log shows successful switch messages
- [x] No memory leaks after 10+ switches
- [x] RTSP stream reflects new dimensions after switch
- [x] High-res photo capture still works after resolution switch
- [x] Photo capture correctly restores to new baseline (not hardcoded 800x800)

## Integration Test

Full workflow test:

```bash
# 1. Query initial state
curl http://192.168.1.91/api/stream/resolution

# 2. Switch to WVGA
curl -X PUT -d '{"resolution":"wvga"}' \
  http://192.168.1.91/api/stream/resolution

# 3. Capture high-res photo
curl -X POST -d '{}' http://192.168.1.91/api/photo/capture
sleep 3

# 4. Verify stream restored to WVGA (not XVGA)
curl http://192.168.1.91/api/stream/resolution

# 5. Download photo
curl -o test.jpg http://192.168.1.91/api/photo/latest.jpg

# 6. Verify JPEG is 1920x1080
identify test.jpg  # Should show: test.jpg JPEG 1920x1080

# 7. Switch back to XVGA
curl -X PUT -d '{"resolution":"xvga"}' \
  http://192.168.1.91/api/stream/resolution

# 8. Capture another photo
curl -X POST -d '{}' http://192.168.1.91/api/photo/capture
sleep 3

# 9. Verify stream restored to XVGA this time
curl http://192.168.1.91/api/stream/resolution
```

Expected: Stream resolution changes as requested, high-res capture works at both resolutions, baseline restoration always returns to current resolution (not hardcoded).

## Web Panel Integration

Once HTTP API is validated, create a simple web interface:

```html
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-P4 Camera Control</title>
</head>
<body>
  <h1>Stream Resolution</h1>
  <div id="current">Loading...</div>
  <button onclick="setResolution('xvga')">800x800 (XVGA)</button>
  <button onclick="setResolution('wvga')">800x640 (WVGA)</button>
  
  <h1>Photo Capture</h1>
  <button onclick="capturePhoto()">Capture 1080p Photo</button>
  <img id="photo" style="max-width:100%">
  
  <script>
    async function updateResolution() {
      const res = await fetch('/api/stream/resolution');
      const data = await res.json();
      document.getElementById('current').textContent = 
        `Current: ${data.current.toUpperCase()} (${data.width}x${data.height})`;
    }
    
    async function setResolution(res) {
      const response = await fetch('/api/stream/resolution', {
        method: 'PUT',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({resolution: res})
      });
      if (response.ok) {
        alert('Resolution changed');
        updateResolution();
      } else {
        alert('Failed: ' + response.statusText);
      }
    }
    
    async function capturePhoto() {
      const response = await fetch('/api/photo/capture', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: '{}'
      });
      if (response.ok) {
        setTimeout(() => {
          document.getElementById('photo').src = 
            '/api/photo/latest.jpg?' + Date.now();
        }, 3000);
      }
    }
    
    updateResolution();
    setInterval(updateResolution, 5000);
  </script>
</body>
</html>
```

Save as `control.html` and serve from SPIFFS or SD card, or inline in a GET `/` handler.
