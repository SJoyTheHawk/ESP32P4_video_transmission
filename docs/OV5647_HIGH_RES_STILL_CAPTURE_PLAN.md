# OV5647 High-Resolution Still Capture Plan

## Goal

Keep the existing low-resolution RTSP/RTP-JPEG stream running at its current
settings while adding an HTTP API that captures a high-resolution JPEG on
demand.

The final interface supports:

- A continuous 800x800 RTSP stream at JPEG quality 50 and 10 FPS.
- A 1920x1080 still-capture mode for initial integration and validation.
- A 2592x1944 (5 MP) still-capture mode as the final maximum-resolution mode.
- A short RTSP frame pause during a still capture, without intentionally
  closing the RTSP session.

## Sensor and driver findings

The camera sensor is the OV5647. Its active array is 2592x1944 pixels and the
sensor supports full-resolution output at up to 15 FPS.

The OV5647 has one image output pipeline. It cannot generate independent
800x800 and 2592x1944 frames at the same time. The application must therefore
temporarily stop low-resolution capture, switch the sensor mode, take the
still image, and restore the streaming mode.

The OV5647 driver bundled with the currently installed Arduino ESP32 core only
exposes these modes:

- 800x1280 RAW8 at 50 FPS
- 800x640 RAW8 at 50 FPS
- 800x800 RAW8 at 50 FPS

The latest upstream Espressif driver also provides 1920x1080 RAW10 at 30 FPS
and 1280x960 RAW10 at 45 FPS, but it does not yet expose 2592x1944. The
application will therefore supply repository-local high-resolution sensor
mode descriptions rather than replacing the complete Arduino core.

## Selected behavior

The existing stream remains unchanged:

| Setting | Streaming value |
| --- | --- |
| Resolution | 800x800 |
| Capture format | RGB565 |
| JPEG quality | 50 |
| Frame rate | 10 FPS |
| Capture buffers | 2 |
| RTSP port | TCP 554 |
| RTP/JPEG port | UDP 5430 |

Still captures use the following settings:

| API value | Resolution | Sensor mode | Capture format | JPEG quality |
| --- | ---: | --- | --- | ---: |
| `1080p` | 1920x1080 | RAW10, up to 30 FPS | YUV420 | 90 |
| `5mp` | 2592x1944 | RAW10, up to 15 FPS | YUV420 | 90 |

YUV420 is selected for still capture to reduce the 5 MP input buffer from
about 9.6 MiB with RGB565 to about 7.2 MiB. Only one high-resolution capture
buffer will be allocated.

## HTTP API

Start an ESP-IDF HTTP server on TCP port 80 after Wi-Fi and camera
initialization.

Supported requests:

```text
GET /api/capture.jpg?resolution=1080p
GET /api/capture.jpg?resolution=5mp
GET /api/capture.jpg
```

Once both implementation stages are complete, an omitted `resolution`
parameter defaults to `5mp`.

A successful response uses:

```text
HTTP/1.1 200 OK
Content-Type: image/jpeg
Cache-Control: no-store
Content-Disposition: inline; filename="ov5647-5mp.jpg"
X-Image-Width: 2592
X-Image-Height: 1944
X-Capture-Time-Ms: <measured duration>
```

Errors use `application/json` and one of these status codes:

| Status | Condition |
| ---: | --- |
| 400 | Unsupported `resolution` value |
| 409 | Another still-capture request is already running |
| 503 | Requested mode is unavailable or memory allocation fails |
| 504 | Frame capture or JPEG encoding times out |
| 500 | Camera mode restoration fails |

The first version is intended for a trusted LAN and does not add
authentication.

## Camera pipeline changes

Add a repository-local camera capture controller around the V4L2 device. It
will own the capture file descriptor, MMAP buffers, selected output format,
and capture timeout. `ESPVideoClass` remains responsible for initializing the
ESP32-P4 MIPI-CSI hardware.

The controller must provide these operations:

- Read and retain the driver-selected 800x800 sensor mode during startup.
- Stop capture and release all MMAP buffers.
- Apply a supplied `esp_cam_sensor_format_t` with
  `VIDIOC_S_SENSOR_FMT` while capture is stopped.
- Reopen capture with either one or two buffers and select RGB565 or YUV420.
- Capture a frame with a bounded dequeue timeout.
- Stop and clean up safely after a partial initialization failure.

The 1920x1080 mode table will be based on Espressif's Apache-licensed upstream
OV5647 implementation. The 2592x1944 table will be derived from the public
OV5647 register documentation and validated against published full-resolution
timing parameters. The expected initial 5 MP configuration is:

- Pixel clock: approximately 87.5 MHz
- Horizontal total size: 2844 pixels
- Vertical total size: 1968 lines
- MIPI line rate: approximately 437.5 Mbps per lane
- Output: 2592x1944 RAW10 at up to 15 FPS

The complete table must include reset, PLL, crop, output size, timing,
exposure, gain, and MIPI configuration. It must not depend on register state
left by the preceding low-resolution mode.

## JPEG encoder changes

Extend `JpegEncoderClass` so that it can be stopped and recreated for a new
resolution, input format, quality, output capacity, and timeout.

Use these initial output-buffer limits:

| Mode | Maximum JPEG buffer | Encoding timeout |
| --- | ---: | ---: |
| 800x800 stream | Existing allocation | 40 ms |
| 1920x1080 still | 2 MiB | 500 ms |
| 2592x1944 still | 4 MiB | 500 ms |

The still encoder must accept YUV420 input and generate a quality-90 JPEG. It
must validate the SOI and EOI markers before returning the image.

After encoding, detach the completed JPEG buffer from the high-resolution
encoder. Restore the low-resolution camera and encoder before sending the
HTTP body. This prevents slow Wi-Fi transfer of a large JPEG from extending
the RTSP interruption.

## Still-capture transaction

The HTTP handler and the streaming loop share the camera and hardware JPEG
engine, so all state transitions must be serialized by a mutex and a separate
atomic busy flag.

For each still request:

1. Atomically claim the still-capture operation. Return 409 if it is busy.
2. Lock the camera pipeline and make the RTSP loop skip new frame capture.
3. Stop low-resolution capture and release its two buffers and JPEG encoder.
4. Apply the requested high-resolution OV5647 sensor mode.
5. Start capture with one YUV420 buffer and initialize the quality-90 encoder.
6. Discard the first three valid frames so exposure and the new mode can
   settle.
7. Capture and encode the next frame, then detach the resulting JPEG buffer.
8. Stop high-resolution capture and release all high-resolution resources.
9. Reapply the saved 800x800 sensor mode, restore two RGB565 buffers, and
   recreate the quality-50 streaming encoder.
10. Discard two restored low-resolution frames before allowing RTSP output.
11. Unlock the pipeline and resume RTSP frame delivery.
12. Send the detached JPEG as the HTTP response and free it afterward.

The RTSP server and client session remain active during the transaction. Only
media frame production pauses.

Every failure after step 3 must enter the same restoration path before an HTTP
error is returned. If restoration itself fails, report status 500, keep the
HTTP service alive, mark the camera unavailable, and log the exact failed
operation.

## Memory management

Log total PSRAM, free PSRAM, and largest free block during startup and before
and after each still-capture transaction.

Before changing modes, verify that enough memory is available for the target
YUV420 buffer, bounded JPEG output buffer, driver overhead, and at least 1 MiB
of safety reserve. Allocation failures must restore streaming and return 503.

Do not silently reduce resolution or JPEG quality when memory is insufficient.
This makes API behavior deterministic and exposes whether a particular board
variant has enough PSRAM for 5 MP quality 90.

## Implementation stages

### Stage 1: Pipeline and 1080p capture

- Add the synchronized V4L2 capture controller and reusable JPEG encoder
  lifecycle.
- Save and restore the current 800x800 mode.
- Add the upstream 1920x1080 RAW10 mode.
- Implement the HTTP server and `resolution=1080p` response.
- Confirm that RTSP resumes without reconnecting after repeated captures.
- Validate dimensions, color order, orientation, field of view, JPEG markers,
  memory use, and timeout recovery.

### Stage 2: Full 5 MP capture

- Add and document the 2592x1944 RAW10 register profile.
- Validate sensor timing and MIPI-CSI reception on the physical board.
- Implement `resolution=5mp` and make it the default when the parameter is
  omitted.
- Confirm quality-90 encoding within the 4 MiB output limit.
- Measure the RTSP pause and PSRAM high-water mark.

### Stage 3: Hardening and documentation

- Exercise all API error responses and restoration paths.
- Add serial diagnostics for mode switches, capture duration, JPEG size,
  available PSRAM, and restoration duration.
- Document the endpoint in the project README without exposing Wi-Fi
  credentials.
- Record hardware acceptance results and the detected PSRAM capacity.

## Test plan

### Build and static checks

- Compile the normal Wi-Fi/RTSP build for the configured ESP32-P4 board.
- Compile the camera-only build if `EXCLUDE_WIFI` remains supported.
- Confirm that the existing RTSP packetizer and its fixed 800x800 stream
  assumptions are not given a high-resolution still frame.

### Functional tests

- Request 1080p and verify that a standard JPEG decoder reports 1920x1080.
- Request 5 MP and verify that a standard JPEG decoder reports 2592x1944.
- Photograph a color chart to validate Bayer order and rule out red/blue or
  green-channel errors.
- Verify orientation and full sensor field of view against the 800x800 stream.
- Verify that still capture works with and without an active RTSP client.

### Streaming and reliability tests

- Keep an RTSP receiver connected while alternating 1080p and 5 MP requests.
- Run at least 100 still captures without rebooting the board.
- Confirm that the RTSP session stays established and automatically resumes.
- Target a media pause below 2 seconds for 1080p and below 3 seconds for 5 MP.
- Confirm that free PSRAM returns to its baseline after every request and does
  not trend downward.
- Disconnect the HTTP client during response transfer and verify that the JPEG
  response buffer is still freed.

### Failure tests

- Send an unsupported resolution and verify status 400.
- Send overlapping still requests and verify status 409.
- Force an allocation failure and verify status 503 plus RTSP restoration.
- Force a capture or encoding timeout and verify status 504 plus restoration.
- Simulate a sensor-mode failure and verify that the saved streaming mode is
  reapplied.

## Acceptance criteria

The feature is accepted when:

- The existing 800x800, quality-50, 10 FPS RTSP stream remains compatible with
  the current receiver.
- The API returns decodable 1920x1080 and 2592x1944 quality-90 JPEG images.
- A connected RTSP client resumes after every successful or failed snapshot
  transaction without reconnecting.
- Repeated still captures do not leak buffers, encoder handles, or PSRAM.
- Unsupported, busy, memory, timeout, and restoration errors return the
  documented status codes.
- The physical board's measured PSRAM capacity and 5 MP memory margin are
  recorded. If 5 MP does not fit, the API fails explicitly rather than
  changing the requested output.

## Out of scope

- Simultaneous dual sensor output at different resolutions
- Multiple concurrent still captures
- Authentication or access from an untrusted network
- Changing the RTSP transport, packetizer, ports, or client count
- Runtime selection of JPEG quality
- Continuous 5 MP streaming

## References

- [OV5647 camera module specification](https://files.seeedstudio.com/wiki/OV5647_Series_Camera_Module/OV5647-62.pdf)
- [OV5647 datasheet](https://www.welectron.com/mediafiles/productimg/arducam/Image_Sensor/OV5647_DS.pdf)
- [Espressif upstream OV5647 driver](https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/sensors/ov5647/ov5647.c)
- [Linux OV5647 driver](https://kernel.googlesource.com/pub/scm/linux/kernel/git/klassert/ipsec-next/+/refs/heads/master/drivers/media/i2c/ov5647.c)
- [ESP32-P4 JPEG encoder documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/jpeg.html)
