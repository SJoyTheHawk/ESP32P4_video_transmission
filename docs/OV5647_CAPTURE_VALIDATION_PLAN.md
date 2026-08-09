# OV5647 Camera and Wireless Validation Plan

## Summary

Adapt the manufacturer ESP-IDF example for the ESP32-P4 camera-board pinout and validate the camera and wireless path in small, testable milestones. There is no LCD on the target hardware, so camera validation will use serial diagnostics and captured-buffer checks.

The manufacturer project runs on ESP-IDF 5.5.5 and already includes the OV5647 driver. The sensor sends RAW8 over two MIPI-CSI lanes. The P4 will capture frames and later encode them to JPEG before forwarding them through the ESP32-C6 Wi-Fi controller.

## Milestone Order

1. [x] Camera bring-up and frame validation without an LCD.
2. [x] P4-to-C6 SDIO and C6 Wi-Fi connectivity.
3. [ ] JPEG photo stream from the P4 to the PC receiver.
4. [ ] Runtime OV5647 resolution switching.
5. [ ] Runtime JPEG quality and remote controls.
6. [ ] AI pipeline measurements and optional 5 MP support.

## Implementation Steps and Status

### Step 1 - Headless camera bring-up (complete)

Implemented in [`mipi_csi_camera.ino`](../mipi_csi_camera/mipi_csi_camera.ino):

1. Compile only for ESP32-P4 with ESP-IDF 5.4 or newer.
2. Configure the OV5647 SCCB/I2C control bus using SCL GPIO29 and SDA GPIO28.
3. Configure the camera reset and power-down signals on GPIO26 and GPIO27.
4. Initialize the ESP-Video MIPI-CSI pipeline.
5. Open `/dev/video0` and allocate two capture buffers.
6. Request RGB565 output and start the CSI stream.
7. Dequeue frames in the main loop, validate the payload size, calculate a sampled hash, and report dimensions and capture rate over Serial at 115200 baud.
8. Keep the camera path headless; no LCD or display task is required.

This step validates that frames reach the P4 capture buffer. It does not yet select a resolution at runtime, produce JPEG, or transmit data over Wi-Fi. The code implementation is complete; flashing the board and confirming the serial output remains a hardware acceptance test.

### Step 2 - P4-to-C6 wireless link (complete)

The selected architecture uses the ESP-Hosted support already included in the ESP32 Arduino core. A new P4 transport package is not required. The PC side has a separate, dependency-free Python receiver for link tests and later frame ingestion.

1. Configure the P4 SD2 SDIO pins from the board mapping: GPIO49-52 for D0-D3, GPIO53 for CMD, and GPIO54 for CLK.
2. Drive the board-specific C6 wake signal on GPIO12 and use GPIO19 as the C6 enable/reset signal.
3. Call the Arduino core's `WiFi.setPins()` and `WiFi.STA.begin()` to initialize ESP-Hosted-MCU.
4. Exchange a control/status request before involving the camera path.
5. Confirm that the C6 associates with Wi-Fi and can complete a basic TCP/HTTP connectivity test.
6. Log link state, peer address, and round-trip status on the P4 serial console.
7. Run [`tools/pc_receiver.py`](../tools/pc_receiver.py) on the PC. It provides `GET /health`, `POST /echo`, and a future-ready `POST /stream` endpoint using only Python's standard library.

For the first hardware test, start the receiver with `python3 tools/pc_receiver.py --port 8080`, then fill in the Wi-Fi credentials and PC address in `mipi_csi_camera.ino`. The sketch initializes the SDIO transport even when the SSID is empty, but it skips association and the PC request in that case.

Hardware validation completed successfully:

- ESP-Hosted SDIO transport initialized.
- The ESP32-C6 associated with Wi-Fi and assigned `192.168.1.91` to the P4.
- The PC receiver returned `HTTP 200` for `/health`.
- Camera capture continued successfully after wireless initialization.

The C6 firmware-version RPC reported a timeout during startup. It did not prevent networking, but the host and coprocessor firmware versions should be recorded and checked during reliability testing.

### Step 3 - JPEG photo stream to the PC

This milestone creates the first end-to-end image path. The P4 will capture a frame, encode it as JPEG, and send that complete JPEG to the PC receiver. The receiver's `/stream` endpoint is already available, but the current sketch does not send frames yet.

1. Open the P4 JPEG video device (`/dev/video10`).
2. Feed the captured camera frame into the hardware JPEG encoder.
3. Build a small frame header containing sequence number, dimensions, format, payload length, and checksum.
4. Send one complete JPEG per HTTP POST to the PC receiver's `/stream` endpoint.
5. Save received payloads on the PC and verify JPEG markers, dimensions, sequence numbers, and checksums.
6. Measure capture-to-PC latency, transfer time, encoded size, and dropped frames.
7. Keep this as an individual JPEG upload sequence; it is not an MJPEG upload stream.

The first implementation should use a conservative frame rate and one in-flight request so buffer ownership and network behavior are easy to observe. Higher-rate streaming can be added after the basic path is reliable.

### Step 4 - Runtime resolution switching

1. Add an application mode enum for the three sensor-supported modes.
2. Expose a narrow mode-selection function that uses the OV5647 driver-owned descriptors.
3. Stop the CSI stream before changing sensor mode.
4. Recreate capture buffers using the new dimensions.
5. Restart the stream and read back the active format.
6. Reject unsupported mode indices without disturbing the active stream.

### Step 5 - Runtime JPEG quality and remote controls

1. Set quality through `V4L2_CID_JPEG_COMPRESSION_QUALITY`.
2. Compare encoded sizes and image quality at several settings.
3. Add acknowledgements and timeout handling to the PC upload path.
4. Add commands for resolution, JPEG quality, frame rate, and start/stop capture.
5. Apply commands only at defined frame boundaries and report the active settings.

### Step 6 - AI-oriented validation

1. Measure capture-to-server latency and dropped-frame rate.
2. Compare RGB565 and JPEG paths for memory use and throughput.
3. Select an operating mode based on AI accuracy, bandwidth, and latency.
4. Add the 5 MP OV5647 mode only after the current driver and memory requirements are verified.

## Supported Camera Modes

The bundled OV5647 driver supports these discrete sensor modes:

| Mode index | Resolution | Sensor output | Frame rate |
| ---: | --- | --- | ---: |
| 0 | 800x1280 | MIPI 2-lane RAW8 | 50 FPS |
| 1 | 800x640 | MIPI 2-lane RAW8 | 50 FPS |
| 2 | 800x800 | MIPI 2-lane RAW8 | 50 FPS |

The default build selects mode index 2 (`800x800`). Arbitrary resolutions such as VGA, SVGA, or UXGA are not provided by this driver.

## Implementation Changes

### Board camera configuration

- Use the existing shared I2C bus on GPIO29 (SCL) and GPIO28 (SDA).
- Configure the camera reset pin as GPIO26 and power-down pin as GPIO27, matching the board pinout CSV.
- Keep the MIPI-CSI data and clock lanes on the connector's dedicated differential pins.
- Make the checked-in Kconfig defaults match these board values so the project remains correct even if the shared I2C bus is later initialized inside `esp_video`.

### Capture and mode selection

- Keep `/dev/video0` as the MIPI-CSI capture device. Use RGB565 as the first validation output format because the P4 ISP can convert the OV5647 RAW8 input without requiring the JPEG encoder.
- Remove the LCD-specific display task from the application capture loop. The capture task should dequeue a frame, log its metadata, validate its buffer contents, and requeue it.
- Add an internal mode enum and a camera mode-selection function covering the three OV5647 modes.
- Use the sensor format descriptors owned by the OV5647 driver; do not duplicate its register tables in the application.
- Add a narrow index-based wrapper around the existing private sensor-format mechanism so the application can select a driver-owned descriptor by mode index.
- Perform a mode change only after stopping capture:
  1. Stop the V4L2 stream.
  2. Apply the selected OV5647 sensor format.
  3. Recreate the V4L2 capture buffers using the new dimensions.
  4. Restart the CSI stream.
  5. Read back and log the active format.
- Do not use ordinary `VIDIOC_S_FMT` as the resolution selector. In the CSI driver it changes the P4/ISP output pixel format while requiring the existing sensor width and height.

### Validation output

- Add serial logs for OV5647 sensor ID, selected mode, `VIDIOC_G_FMT` width/height, buffer size, `bytesused`, and measured frame rate.
- Validate each dequeued buffer with nonzero length, bounds checking, and a lightweight checksum or sample-byte diagnostic. This provides frame-integrity evidence without a display.
- Reject unsupported mode indices without changing the active stream.

### P4-to-C6 wireless connection

- Add ESP-Hosted-MCU over the board's 4-bit SD2 SDIO connection:
  - P4 GPIO49-54 for SD2 D0-D3, CMD, and CLK.
  - P4 GPIO12 for C6 wake-up.
  - P4 GPIO19 for C6 enable.
- Keep the C6 responsible for Wi-Fi station mode and TCP/IP sockets.
- First validate the link with a status exchange and a simple network connectivity test; do not send camera frames until the link is stable.
- Record the negotiated Wi-Fi state, peer address, and round-trip status response in the serial log.

## Test Plan

1. Build and flash the adapted project with the default `800x800` mode and no LCD dependencies.
2. Confirm sensor detection reports PID `0x5647`.
3. Confirm serial diagnostics report nonzero, in-bounds frame buffers and stable capture timing.
4. [x] Confirm P4-C6 SDIO initialization and status exchange.
5. [x] Confirm the C6 associates with the configured Wi-Fi network and completes a simple TCP/HTTP connectivity test.
6. Send a JPEG frame to the PC `/stream` endpoint and verify that it can be opened as an image.
7. Verify sequence numbers, payload lengths, checksums, and receive timestamps.
8. Switch at runtime to `800x640`, then `800x1280`, then back to `800x800`.
9. For every transition, verify the reported dimensions, JPEG metadata, nonzero frame data, and absence of CSI or buffer errors.
10. Send an invalid mode index and verify that it is rejected while the previous mode continues running.
11. Repeat camera mode changes and wireless status exchanges to detect stale MMAP buffers, memory leaks, or SDIO recovery failures.

## Deferred Work

- Complete the Step 3 P4 JPEG-to-PC path.
- Expose JPEG compression quality through `V4L2_CID_JPEG_COMPRESSION_QUALITY` in Step 5 and measure the resulting JPEG sizes.
- Add remote controls and acknowledgements after the basic JPEG path is stable.
- The existing reference protocol uses one JPEG per HTTP POST; it is not an MJPEG upload stream.

## Relevant Existing Sources

- [`mipi_csi_camera.ino`](../mipi_csi_camera/mipi_csi_camera.ino) contains the P4 camera and ESP-Hosted link validation flow.
- [`pc_receiver.py`](../tools/pc_receiver.py) is the local HTTP health/echo/frame endpoint for staged testing.
- [`mipi_cam.c`](../reference/17_mipicamera/main/APP/MIPI_CAM/mipi_cam.c) shows the existing `/dev/video0` capture path; its LCD-specific task will be replaced by a headless capture task.
- [`app_video.c`](../reference/17_mipicamera/main/APP/MIPI_CAM/app_video.c) initializes the CSI camera and V4L2 buffers.
- [`sdkconfig`](../reference/17_mipicamera/sdkconfig) contains the current generic pin defaults and OV5647 mode selection.
- [`ov5647.c`](../reference/17_mipicamera/managed_components/espressif__esp_cam_sensor/sensors/ov5647/ov5647.c) defines the three sensor descriptors and register tables.
- [`esp_video_csi_device.c`](../reference/17_mipicamera/managed_components/espressif__esp_video/src/device/esp_video_csi_device.c) shows the distinction between sensor format selection and CSI/ISP output format selection.

## Assumptions

- GPIO26 and GPIO27 are physically connected to the OV5647 reset and power-down signals as listed in the board CSV.
- The target board has no LCD; the first milestone is headless camera capture validated through serial logs and buffer checks.
- JPEG quality belongs to the P4 hardware encoder, not to OV5647 sensor registers.
- Wireless connectivity is the second milestone; the first JPEG-to-PC path is the third milestone, before runtime mode switching.
- ESP32-C6 Wi-Fi and SDIO integration is validated with control/status traffic before camera frames are sent.
