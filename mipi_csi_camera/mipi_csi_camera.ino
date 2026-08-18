/**
 * OV5647 MIPI-CSI to RTSP/RTP-JPEG prototype for ESP32-P4.
 *
 * RTSP control uses TCP port 554. RFC 2435 RTP/JPEG media uses UDP port
 * 5430 and RTCP reports are accepted and discarded on UDP port 5431.
 */

// #define EXCLUDE_WIFI  // Uncomment for camera-only bring-up.

#include "Arduino.h"
#include <ESP_Video.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include "capture_controller.h"
#include "jpeg_output_buffer.h"
#include "photo_store.h"
#include "photo_api.h"
#include "settings_manager.h"
#include "auth_manager.h"

#ifndef EXCLUDE_WIFI
#include <WiFi.h>
#include "src/rtsp_server/ESP32-RTSPServer.h"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#if CONFIG_IDF_TARGET_ESP32P4
#define EXAMPLE_MIPI_CSI_SCCB_I2C_PORT     0
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN 29
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN 28
#define EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ     100000
#define EXAMPLE_MIPI_CSI_SENSOR_RESET_PIN  26
#define EXAMPLE_MIPI_CSI_SENSOR_PWDN_PIN   27

#ifndef EXCLUDE_WIFI
#define EXAMPLE_C6_SDIO_D0_PIN  49
#define EXAMPLE_C6_SDIO_D1_PIN  50
#define EXAMPLE_C6_SDIO_D2_PIN  51
#define EXAMPLE_C6_SDIO_D3_PIN  52
#define EXAMPLE_C6_SDIO_CMD_PIN 53
#define EXAMPLE_C6_SDIO_CLK_PIN 54
#define EXAMPLE_C6_WAKE_PIN     12
#define EXAMPLE_C6_ENABLE_PIN   19

// const char *kWifiSsid = "XIMS2";
// const char *kWifiPassword = "Ns203Ns203.";
const char *kWifiSsid = "South Park";
const char *kWifiPassword = "qwerasdf";
const uint32_t kWifiConnectTimeoutMs = 15000;
const uint32_t kWifiReconnectIntervalMs = 5000;
const uint16_t kRtspPort = 554;
const uint16_t kRtpVideoPort = 5430;
#endif
#else
#error "The selected target SoC is not supported"
#endif

ESPVideoClass video;
CaptureController capture_controller;
PhotoStore photo_store;
PhotoApi photo_api;
SettingsManager settings_manager;
CameraSettings settings;
AuthManager auth_manager;

static bool capturePhotoForApi(void *) {
  HighResStillCandidate candidate;
  if (!capture_controller.captureHighResStill(&candidate)) {
    Serial.printf("photo capture status=failed controller_state=%s\n",
                  capture_controller.stateName());
    return false;
  }
  DetachedJpegOutputBuffer detached = candidate.jpeg;
  candidate.jpeg = {};
  PhotoBlob *photo = PhotoBlob::create(
    detached, candidate.size, candidate.width, candidate.height,
    candidate.quality, candidate.captured_ms);
  if (photo == nullptr) {
    Serial.println("photo capture status=failed reason=photo-allocation");
    return false;
  }
  const bool published = photo_store.publish(photo);
  if (!published) {
    Serial.println("photo capture status=failed reason=photo-publish");
  }
  return published;
}

const size_t kCaptureBufferCount = 2;
const uint32_t kJpegQuality = 50;
const uint32_t kJpegIntervalMs = 100;
const uint32_t kCaptureDequeueTimeoutMs = 5000;

#if 0
static bool logCameraBaseline() {
  const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    Serial.println("baseline: failed to open CSI video device");
    return false;
  }

  esp_cam_sensor_format_t sensor_format = {};
  const bool sensor_format_ready =
    ioctl(fd, VIDIOC_G_SENSOR_FMT, &sensor_format) == 0;
  if (sensor_format_ready) {
    const char *name = sensor_format.name == nullptr
                       ? "unnamed" : sensor_format.name;
    Serial.printf(
      "sensor baseline mode=%s output=%ux%u format=%d fps=%u xclk=%d "
      "mipi_clk=%lu lanes=%lu\n",
      name, sensor_format.width, sensor_format.height,
      static_cast<int>(sensor_format.format), sensor_format.fps,
      sensor_format.xclk,
      static_cast<unsigned long>(sensor_format.mipi_info.mipi_clk),
      static_cast<unsigned long>(sensor_format.mipi_info.lane_num));
  } else {
    Serial.println("baseline: failed to read active sensor format");
  }

  struct v4l2_format video_format = {};
  video_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const bool video_format_ready = ioctl(fd, VIDIOC_G_FMT, &video_format) == 0;
  if (video_format_ready) {
    Serial.printf(
      "video baseline width=%lu height=%lu fourcc=" V4L2_FMT_STR
      " bytes_per_line=%lu size_image=%lu\n",
      static_cast<unsigned long>(video_format.fmt.pix.width),
      static_cast<unsigned long>(video_format.fmt.pix.height),
      V4L2_FMT_STR_ARG(video_format.fmt.pix.pixelformat),
      static_cast<unsigned long>(video_format.fmt.pix.bytesperline),
      static_cast<unsigned long>(video_format.fmt.pix.sizeimage));
  } else {
    Serial.println("baseline: failed to read active video format");
  }

  if (video_format_ready) {
    probeVgaOutputFormat(fd, video_format);
  }
  close(fd);
  return sensor_format_ready && video_format_ready;
}

// ESP-Video resets the per-device timeout on every open.  Configure a second
// reference after capture_dev.begin() so the setting remains on the shared
// driver object while capture_dev owns its descriptor.
static bool configureActiveCaptureDequeueTimeout() {
  const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    Serial.printf("capture timeout status=failed open_errno=%d\n", errno);
    return false;
  }

  const bool ready = configureCaptureDequeueTimeout(fd);
  close(fd);
  return ready;
}

static bool runCaptureTimeoutRecoveryTest() {
  ESPVideoBufferClass held[kCaptureBufferCount];
  size_t held_count = 0;
  uint32_t timeout_elapsed_us = 0;
  int driver_errno = 0;
  bool timeout_observed = false;

  // Some sensor/driver combinations expose only one ready buffer at a time.
  // Treat the first failed dequeue after at least one held buffer as the
  // starvation event; if both buffers are ready, perform one extra dequeue.
  while (held_count < kCaptureBufferCount) {
    const uint32_t start_us = micros();
    errno = 0;
    ESPVideoBufferClass candidate = capture_dev.captureBuffer();
    const uint32_t elapsed_us = micros() - start_us;
    if (!candidate.valid()) {
      if (held_count == 0) {
        Serial.printf("capture timeout test status=failed stage=hold-buffer index=0 errno=%d\n",
                      errno);
        return false;
      }
      timeout_elapsed_us = elapsed_us;
      driver_errno = errno;
      timeout_observed = true;
      break;
    }
    held[held_count++] = static_cast<ESPVideoBufferClass &&>(candidate);
  }

  if (!timeout_observed) {
    const uint32_t start_us = micros();
    errno = 0;
    ESPVideoBufferClass timed_out = capture_dev.captureBuffer();
    timeout_elapsed_us = micros() - start_us;
    driver_errno = errno;
    timeout_observed = !timed_out.valid();
  }

  const bool bounded = timeout_observed
                       && timeout_elapsed_us >= 4500000U
                       && timeout_elapsed_us <= 6500000U;
  const bool expected_driver_result = driver_errno == EPERM
                                      || driver_errno == ETIMEDOUT;

  for (size_t i = 0; i < held_count; ++i) {
    held[i].end();
  }

  ESPVideoBufferClass recovery = capture_dev.captureBuffer();
  const bool recovered = recovery.valid();
  if (recovered) {
    recovery.end();
  }

  const bool ready = bounded && expected_driver_result && recovered;
  Serial.printf(
    "capture timeout test status=%s injection=hold-available-buffers "
    "requested_ms=%lu elapsed_ms=%llu held_buffers=%u driver_errno=%d reported_errno=%d "
    "stream_started=%s recovery_frame=%s\n",
    ready ? "passed" : "failed",
    static_cast<unsigned long>(kCaptureDequeueTimeoutMs),
    static_cast<unsigned long long>(timeout_elapsed_us / 1000),
    static_cast<unsigned>(held_count), driver_errno,
    (bounded && expected_driver_result) ? ETIMEDOUT : driver_errno,
    capture_dev.isCaptureStarted() ? "yes" : "no",
    recovered ? "valid" : "invalid");
  return ready;
}
#endif

#ifndef EXCLUDE_WIFI
RTSPServer rtsp_server;
bool rtsp_server_ready = false;
bool firmware_ready = false;
uint32_t last_wifi_reconnect_ms = 0;
uint32_t stream_sequence = 0;
uint32_t dropped_frames = 0;
uint32_t maximum_send_us = 0;
uint32_t maximum_cycle_us = 0;

static bool initializeWirelessLink() {
#if CONFIG_ESP_HOSTED_ENABLED
  pinMode(EXAMPLE_C6_WAKE_PIN, OUTPUT);
  digitalWrite(EXAMPLE_C6_WAKE_PIN, HIGH);

  if (!WiFi.setPins(
        EXAMPLE_C6_SDIO_CLK_PIN,
        EXAMPLE_C6_SDIO_CMD_PIN,
        EXAMPLE_C6_SDIO_D0_PIN,
        EXAMPLE_C6_SDIO_D1_PIN,
        EXAMPLE_C6_SDIO_D2_PIN,
        EXAMPLE_C6_SDIO_D3_PIN,
        EXAMPLE_C6_ENABLE_PIN)) {
    Serial.println("failed to configure ESP-Hosted SDIO pins");
    return false;
  }
  // Configure this before STA.begin() so Arduino applies WIFI_PS_NONE on the
  // STA-start event. RTP/JPEG is emitted in short UDP bursts and needs the C6
  // radio awake to reduce observed packet loss.
  if (!WiFi.setSleep(false)) {
    Serial.println("failed to disable Wi-Fi modem power save");
    return false;
  }
  if (!WiFi.STA.begin()) {
    Serial.println("failed to initialize ESP-Hosted Wi-Fi transport");
    return false;
  }

  if (kWifiSsid[0] == '\0') {
    Serial.println("set kWifiSsid and kWifiPassword before the Wi-Fi test");
    return false;
  }

  if (!WiFi.STA.connect(kWifiSsid, kWifiPassword)) {
    Serial.println("failed to start Wi-Fi association");
    return false;
  }

  const uint32_t start = millis();
  while (WiFi.STA.status() != WL_CONNECTED
         && millis() - start < kWifiConnectTimeoutMs) {
    delay(250);
  }
  if (WiFi.STA.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi connection timeout, status=%d\n", WiFi.STA.status());
    return false;
  }

  Serial.print("Wi-Fi connected, IP=");
  Serial.println(WiFi.STA.localIP());
  return true;
#else
  Serial.println("ESP-Hosted support is disabled in this Arduino core");
  return false;
#endif
}

static bool startRtspServer() {
  if (!rtsp_server.init(RTSPServer::VIDEO_ONLY, kRtspPort, 0,
                        kRtpVideoPort)) {
    Serial.println("failed to start RTSP server");
    return false;
  }

  Serial.print("RTSP/RTP-JPEG stream ready: rtsp://");
  Serial.print(WiFi.STA.localIP());
  Serial.printf(":%u/\n", kRtspPort);
  return true;
}

static void serviceWirelessLink() {
  if (WiFi.STA.status() == WL_CONNECTED) {
    if (!rtsp_server_ready) {
      rtsp_server_ready = startRtspServer();
    }
    return;
  }

  if (rtsp_server_ready) {
    Serial.println("Wi-Fi lost; stopping RTSP server");
    rtsp_server.deinit();
    rtsp_server_ready = false;
  }

  const uint32_t now = millis();
  if (now - last_wifi_reconnect_ms < kWifiReconnectIntervalMs) {
    return;
  }
  last_wifi_reconnect_ms = now;
  Serial.println("reconnecting Wi-Fi");
  WiFi.STA.connect(kWifiSsid, kWifiPassword);
}

#endif

void setup() {
  Serial.begin(115200);
  delay(200);

#ifndef EXCLUDE_WIFI
  const bool wireless_ready = initializeWirelessLink();
  if (!wireless_ready) {
    return;
  }
#endif

  ESPVideoCamConfigClass cam_config;
  if (!cam_config.begin(
        EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
        EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
        EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
        EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
        EXAMPLE_MIPI_CSI_SENSOR_RESET_PIN,
        EXAMPLE_MIPI_CSI_SENSOR_PWDN_PIN)) {
    Serial.println("failed to configure camera pins");
    return;
  }

  ESPVideoCSIConfigClass csi_config;
  if (!csi_config.begin(cam_config)) {
    Serial.println("failed to configure CSI camera");
    return;
  }
  if (!video.begin(csi_config)) {
    Serial.println("failed to init CSI camera");
    return;
  }
  if (!capture_controller.beginBaseline(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
                                        kCaptureBufferCount, kJpegQuality,
                                        kCaptureDequeueTimeoutMs)) {
    Serial.println("failed to start camera capture");
    return;
  }
  if (!photo_api.begin(&photo_store, capturePhotoForApi)) {
    Serial.println("failed to start HTTP photo API");
    return;
  }
  photo_api.setCaptureController(&capture_controller);
  photo_api.setAuthManager(&auth_manager);
  Serial.println("HTTP photo API ready: port=80 routes=/api/photo/*,/api/stream/*");
  // Phase 8: Load and apply saved settings
  if (!settings_manager.begin()) {
    Serial.println("settings_manager: failed to initialize, using defaults");
    settings.setDefaults();
    capture_controller.switchResolution(settings.stream_resolution);
  } else if (!settings_manager.loadSettings(settings)) {
    settings.setDefaults();
    settings_manager.saveSettings(settings);
    capture_controller.switchResolution(settings.stream_resolution);
  } else {
    // Always apply the selected mode. The rollback build maps every stream
    // resolution to the known-good 800x800 sensor configuration.
    if (!capture_controller.switchResolution(settings.stream_resolution)) {
      settings.stream_resolution = StreamResolution::HD_1280x720;
      settings_manager.saveSettings(settings);
    }
  }
  photo_api.setSettingsManager(&settings_manager, &settings);

#ifndef EXCLUDE_WIFI
  rtsp_server_ready = startRtspServer();
  firmware_ready = true;
#else
  Serial.println("camera ready; RTSP server excluded");
#endif
}

void loop() {
  if (Serial.available() > 0) {
    const int command = Serial.read();
    if (command == 'v' || command == 'V') {
      Serial.println("Switching to VGA (640x480)...");
      if (capture_controller.switchResolution(StreamResolution::VGA_640x480)) {
        Serial.printf("Resolution switch success: now running at %ux%u\n",
                     capture_controller.width(), capture_controller.height());
      } else {
        Serial.println("Resolution switch failed");
      }
    } else if (command == 'h' || command == 'H') {
      Serial.println("Switching to HD (1280x720)...");
      if (capture_controller.switchResolution(StreamResolution::HD_1280x720)) {
        Serial.printf("Resolution switch success: now running at %ux%u\n",
                     capture_controller.width(), capture_controller.height());
      } else {
        Serial.println("Resolution switch failed");
      }
    } else if (command == 'f' || command == 'F') {
      Serial.println("Switching to Full HD (1920x1080)...");
      if (capture_controller.switchResolution(StreamResolution::FHD_1920x1080)) {
        Serial.printf("Resolution switch success: now running at %ux%u\n",
                     capture_controller.width(), capture_controller.height());
      } else {
        Serial.println("Resolution switch failed");
      }
    } else if (command == 'q' || command == 'Q') {
      Serial.printf("Current resolution: %s (%ux%u)\n",
                   capture_controller.resolutionName(
                     capture_controller.getCurrentResolution()),
                   capture_controller.width(),
                   capture_controller.height());
    }
  }
#ifndef EXCLUDE_WIFI
  if (!firmware_ready) {
    delay(1000);
    return;
  }
  serviceWirelessLink();
  if (!rtsp_server_ready || !rtsp_server.readyToSendFrame()) {
    delay(10);
    return;
  }

  const uint32_t frame_start_ms = millis();
  const uint32_t cycle_start_us = micros();
  const uint32_t capture_start_us = micros();
  errno = 0;
  CaptureController::BaselineFrame frame = capture_controller.captureBaselineFrame();
  const uint32_t capture_us = micros() - capture_start_us;
  const int capture_errno = errno;
  if (!frame.valid()) {
    ++dropped_frames;
    Serial.printf(
      "failed to capture buffer errno=%d "
      "capture_us=%lu\n",
      capture_errno,
      static_cast<unsigned long>(capture_us));
    delay(1);
    return;
  }

  const uint32_t width = frame.width();
  const uint32_t height = frame.height();
  const size_t expected_size = static_cast<size_t>(width) * height * 2;
  if (frame.data() == nullptr || frame.size() < expected_size) {
    ++dropped_frames;
    Serial.println("invalid RGB565 frame");
    frame.end();
    delay(1);
    return;
  }

  JpegEncodeResult jpeg;
  const uint32_t encode_start_us = micros();
  const bool encoded = capture_controller.encodeBaselineFrame(frame, &jpeg);
  const uint32_t encode_us = micros() - encode_start_us;
  const bool valid_jpeg = encoded && jpeg.size >= 4
                          && jpeg.data[0] == 0xff && jpeg.data[1] == 0xd8
                          && jpeg.data[jpeg.size - 2] == 0xff
                          && jpeg.data[jpeg.size - 1] == 0xd9;
  frame.end();
  if (!valid_jpeg) {
    ++dropped_frames;
    Serial.println("failed to encode JPEG");
    delay(1);
    return;
  }

  const uint32_t send_start_us = micros();
  const RtpFrameSendResult send_result = rtsp_server.sendRTSPFrame(
    jpeg.data, jpeg.size, kJpegQuality, width, height);
  const uint32_t send_us = micros() - send_start_us;
  const uint32_t cycle_us = micros() - cycle_start_us;
  maximum_send_us = max(maximum_send_us, send_us);
  maximum_cycle_us = max(maximum_cycle_us, cycle_us);
  ++stream_sequence;
  if (!send_result.sent) {
    ++dropped_frames;
  }

  const uint32_t elapsed_ms = millis() - frame_start_ms;
  if (elapsed_ms < kJpegIntervalMs) {
    delay(kJpegIntervalMs - elapsed_ms);
  }
#else
  delay(1000);
#endif
}
#else
void setup() {
  Serial.begin(115200);
  Serial.println("ESP_IDF_VERSION < 5.4.0, this example is not supported");
}

void loop() {
  delay(1000);
}
#endif
