/**
 * OV5647 MIPI-CSI to RTSP/RTP-JPEG prototype for ESP32-P4.
 *
 * RTSP control uses TCP port 554. RFC 2435 RTP/JPEG media uses UDP port
 * 5430 and RTCP reports are accepted and discarded on UDP port 5431.
 */

// #define EXCLUDE_WIFI  // Uncomment for camera-only bring-up.

#include "Arduino.h"
#include <ESP_Video.h>
#include "jpeg_encoder.h"

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

const char *kWifiSsid = "XIMS2";
const char *kWifiPassword = "Ns203Ns203.";
// const char *kWifiSsid = "VPlace7B-4";
// const char *kWifiPassword = "60727269";
const uint32_t kWifiConnectTimeoutMs = 15000;
const uint32_t kWifiReconnectIntervalMs = 5000;
const uint16_t kRtspPort = 554;
const uint16_t kRtpVideoPort = 5430;
#endif
#else
#error "The selected target SoC is not supported"
#endif

ESPVideoClass video;
ESPVideoCaptureDevClass capture_dev;
JpegEncoderClass jpeg_encoder;

const size_t kCaptureBufferCount = 2;
const uint32_t kJpegQuality = 50;
const uint32_t kJpegIntervalMs = 100;

#ifndef EXCLUDE_WIFI
RTSPServer rtsp_server;
bool rtsp_server_ready = false;
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
  if (!WiFi.STA.begin()) {
    Serial.println("failed to initialize ESP-Hosted Wi-Fi transport");
    return false;
  }

  Serial.println("ESP-Hosted SDIO transport initialized");
  if (kWifiSsid[0] == '\0') {
    Serial.println("set kWifiSsid and kWifiPassword before the Wi-Fi test");
    return false;
  }

  Serial.printf("connecting to Wi-Fi SSID '%s'\n", kWifiSsid);
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
  Serial.println();
  Serial.println("OV5647 UDP RTSP/RTP-JPEG prototype");

#ifndef EXCLUDE_WIFI
  const bool wireless_ready = initializeWirelessLink();
  Serial.printf("wireless validation: %s\n",
                wireless_ready ? "ready" : "not ready");
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
  Serial.printf("CSI camera initialized: %s\n",
                video.isCSIInitialized() ? "yes" : "no");

  if (!capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
                         kCaptureBufferCount)) {
    Serial.println("failed to open capture device");
    return;
  }
  if (!capture_dev.setFormat(ESP_VIDEO_FORMAT_RGB565)) {
    Serial.println("failed to set format");
    return;
  }
  if (!capture_dev.startCapture()) {
    Serial.println("failed to start capture");
    return;
  }
  if (!jpeg_encoder.begin(capture_dev.getWidth(), capture_dev.getHeight(),
                          kJpegQuality)) {
    Serial.println("failed to initialize JPEG encoder");
    return;
  }

#ifndef EXCLUDE_WIFI
  rtsp_server_ready = startRtspServer();
#else
  Serial.println("camera ready; RTSP server excluded");
#endif
}

void loop() {
#ifndef EXCLUDE_WIFI
  serviceWirelessLink();
  if (!rtsp_server_ready || !rtsp_server.readyToSendFrame()) {
    delay(10);
    return;
  }

  const uint32_t frame_start_ms = millis();
  const uint32_t cycle_start_us = micros();
  const uint32_t capture_start_us = micros();
  ESPVideoBufferClass frame = capture_dev.captureBuffer();
  const uint32_t capture_us = micros() - capture_start_us;
  if (!frame.valid()) {
    ++dropped_frames;
    Serial.println("failed to capture buffer");
    delay(1);
    return;
  }

  const uint32_t width = frame.getWidth();
  const uint32_t height = frame.getHeight();
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
  const bool encoded = jpeg_encoder.encode(frame.data(), expected_size, &jpeg);
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

  Serial.printf(
    "rtp sequence=%lu bytes=%lu packets=%u capture_us=%lu encode_us=%lu "
    "send_us=%lu cycle_us=%lu max_send_us=%lu max_cycle_us=%lu "
    "sent=%s error=%d dropped=%lu\n",
    static_cast<unsigned long>(stream_sequence),
    static_cast<unsigned long>(jpeg.size), send_result.packetCount,
    static_cast<unsigned long>(capture_us),
    static_cast<unsigned long>(encode_us),
    static_cast<unsigned long>(send_us),
    static_cast<unsigned long>(cycle_us),
    static_cast<unsigned long>(maximum_send_us),
    static_cast<unsigned long>(maximum_cycle_us),
    send_result.sent ? "yes" : "no", send_result.error,
    static_cast<unsigned long>(dropped_frames));

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
