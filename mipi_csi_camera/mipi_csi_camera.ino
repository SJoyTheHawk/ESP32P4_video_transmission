/**
 * MIPI-CSI ESP_Video capture example.
 *
 * This sketch demonstrates the minimal flow for capturing camera frames over a
 * MIPI-CSI interface with the ESP_Video library: initialize the camera sensor
 * via SCCB, open a V4L2-style capture device, start streaming, and dequeue
 * frames in a loop.
 *
 * Requirements:
 * - ESP-IDF >= 5.4.0
 * - CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y
 *
 * Supported target and default board wiring:
 * - ESP32-P4 CB V1.3 with OV5647 on the MIPI-CSI connector
 *   (SCCB I2C port 0, SCL GPIO 29, SDA GPIO 28, reset GPIO 26,
 *    power-down GPIO 27).
 *
 * How it works:
 * 1. setup() configures SCCB (I2C) with ESPVideoCamConfigClass, wraps it in
 *    ESPVideoCSIConfigClass, and calls ESPVideoClass::begin().
 * 2. capture_dev.begin() opens ESP_VIDEO_MIPI_CSI_DEVICE_NAME, requests two
 *    mmap capture buffers, and startCapture() starts streaming.
 * 3. loop() calls ESPVideoCaptureDevClass::captureBuffer() to dequeue the next
 *    frame. On success it prints the buffer pointer and size to Serial; the
 *    buffer is returned to the driver when the ESPVideoBufferClass object is
 *    destroyed at the end of each iteration.
 *
 * Open Serial Monitor at 115200 baud to see capture status, frame metadata,
 * buffer validation, and a lightweight sampled frame hash.
 */

// #define EXCLUDE_WIFI  // Uncomment to disable ESP-Hosted Wi-Fi for bring-up

#include "Arduino.h"
#include <ESP_Video.h>
#include "jpeg_encoder.h"

#ifndef EXCLUDE_WIFI
#include <WiFi.h>
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#if CONFIG_IDF_TARGET_ESP32P4
/**
 * @brief ESP32-P4 CB V1.3 camera wiring.
 */
#define EXAMPLE_MIPI_CSI_SCCB_I2C_PORT    0
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN 29
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN 28
#define EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ    100000
#define EXAMPLE_MIPI_CSI_SENSOR_RESET_PIN 26
#define EXAMPLE_MIPI_CSI_SENSOR_PWDN_PIN  27

#ifndef EXCLUDE_WIFI
/**
 * @brief ESP32-P4 CB V1.3 connection to the ESP32-C6 Wi-Fi coprocessor.
 *
 * The pin order follows the board CSV: D0, D1, D2, D3, CMD, CLK. The
 * Arduino ESP-Hosted API uses the separate CLK, CMD, D0...D3 argument order.
 */
#define EXAMPLE_C6_SDIO_D0_PIN       49
#define EXAMPLE_C6_SDIO_D1_PIN       50
#define EXAMPLE_C6_SDIO_D2_PIN       51
#define EXAMPLE_C6_SDIO_D3_PIN       52
#define EXAMPLE_C6_SDIO_CMD_PIN      53
#define EXAMPLE_C6_SDIO_CLK_PIN      54
#define EXAMPLE_C6_WAKE_PIN          12
#define EXAMPLE_C6_ENABLE_PIN        19

// Fill these values before testing Wi-Fi association.
const char *kWifiSsid = "";
const char *kWifiPassword = "";
const uint32_t kWifiConnectTimeoutMs = 15000;
const char *kReceiverHost = "192.168.1.152";
const uint16_t kReceiverPort = 5001;
#endif
#else
#error "The selected target SoC is not supported"
#endif

ESPVideoClass video;
ESPVideoCaptureDevClass capture_dev;
/**
 * @brief Number of capture buffers, buffer > 2 for double buffering, this can avoid frame dropping
 */
const size_t kCaptureBufferCount = 2;
const uint32_t kFrameLogInterval = 50;
const size_t kFrameSampleCount = 64;
const uint32_t kJpegQuality = 80;
const uint32_t kJpegIntervalMs = 1000;

JpegEncoderClass jpeg_encoder;
#ifndef EXCLUDE_WIFI
WiFiClient stream_client;
#endif

static uint32_t sampleFrameHash(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0) {
    return 0;
  }

  uint32_t hash = 2166136261u;
  size_t sampleCount = min(length, kFrameSampleCount);
  size_t stride = max(static_cast<size_t>(1), length / sampleCount);

  for (size_t i = 0; i < sampleCount; ++i) {
    size_t offset = min(i * stride, length - 1);
    hash ^= data[offset];
    hash *= 16777619u;
  }

  return hash;
}

#ifndef EXCLUDE_WIFI
static bool writeAll(WiFiClient &client, const uint8_t *data, size_t size) {
  while (size > 0) {
    size_t written = client.write(data, size);
    if (written == 0) {
      return false;
    }
    data += written;
    size -= written;
  }
  return true;
}

static void writeUint16Be(uint8_t *output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

static void writeUint32Be(uint8_t *output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

static bool sendJpegFrame(const JpegEncodeResult &jpeg, uint32_t sequence,
                          uint16_t width, uint16_t height) {
  uint8_t header[16] = {'J', 'P', 'G', '0'};
  writeUint32Be(header + 4, sequence);
  writeUint16Be(header + 8, width);
  writeUint16Be(header + 10, height);
  writeUint32Be(header + 12, jpeg.size);

  return writeAll(stream_client, header, sizeof(header))
         && writeAll(stream_client, jpeg.data, jpeg.size);
}

static bool initializeWirelessLink() {
#if CONFIG_ESP_HOSTED_ENABLED
  // GPIO12 is the board-specific C6 wake signal. ESP-Hosted's Arduino API
  // exposes the SDIO bus and reset/enable pin, but not this auxiliary signal.
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

  uint32_t start = millis();
  while (WiFi.STA.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) {
    delay(250);
  }

  if (WiFi.STA.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi connection timeout, status=%d\n", WiFi.STA.status());
    return false;
  }

  Serial.print("Wi-Fi connected, IP=");
  Serial.println(WiFi.STA.localIP());

  Serial.printf("connecting to JPEG receiver %s:%u\n", kReceiverHost,
                kReceiverPort);
  if (!stream_client.connect(kReceiverHost, kReceiverPort)) {
    Serial.println("failed to connect to JPEG receiver");
    return false;
  }
  Serial.println("JPEG receiver connected");
  return true;
#else
  Serial.println("ESP-Hosted support is disabled in this Arduino core");
  return false;
#endif
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("OV5647 headless MIPI-CSI capture validation");
  Serial.println("1 FPS continuous JPEG TCP transmission test enabled");

#ifndef EXCLUDE_WIFI
  bool wirelessReady = initializeWirelessLink();
  Serial.printf("wireless validation: %s\n", wirelessReady ? "ready" : "not ready");
  if (!wirelessReady) {
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
  Serial.printf("CSI camera initialized: %s\n", video.isCSIInitialized() ? "yes" : "no");

  if (!capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, kCaptureBufferCount)) {
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

  Serial.println("OV5647 capture and 1 FPS JPEG encode/send test started");
}

void loop() {
  if (!capture_dev.isOpened() || !capture_dev.isCaptureStarted()) {
    delay(1000);
    return;
  }

  ESPVideoBufferClass frame = capture_dev.captureBuffer();
  if (!frame.valid()) {
    Serial.println("failed to capture buffer");
    delay(10);
    return;
  }

  static uint32_t frameCount = 0;
  static uint32_t invalidFrameCount = 0;
  static uint32_t lastLogFrame = 0;
  static uint32_t lastLogMillis = millis();
  static uint32_t lastJpegMillis = 0;
  static uint32_t jpegSequence = 0;

  const uint32_t width = frame.getWidth();
  const uint32_t height = frame.getHeight();
  const size_t payloadSize = frame.size();
  const size_t expectedRgb565Size = static_cast<size_t>(width) * height * 2;
  const bool validData = frame.data() != nullptr && payloadSize > 0;
  const bool validSize = validData && payloadSize >= expectedRgb565Size;
  const uint32_t sampleHash = validData ? sampleFrameHash(frame.data(), payloadSize) : 0;

  ++frameCount;
  if (!validSize) {
    ++invalidFrameCount;
  }

  uint32_t now = millis();
  if (validSize && (lastJpegMillis == 0 || now - lastJpegMillis >= kJpegIntervalMs)) {
    lastJpegMillis = now;
    JpegEncodeResult jpeg;
    uint32_t encodeStartUs = micros();
    bool encoded = jpeg_encoder.encode(frame.data(), expectedRgb565Size, &jpeg);
    uint32_t encodeUs = micros() - encodeStartUs;
    bool validJpeg = encoded && jpeg.size >= 4
                     && jpeg.data[0] == 0xff && jpeg.data[1] == 0xd8
                     && jpeg.data[jpeg.size - 2] == 0xff
                     && jpeg.data[jpeg.size - 1] == 0xd9;
    ++jpegSequence;
    Serial.printf("jpeg sequence=%lu bytes=%lu encode_us=%lu valid=%s\n",
                  static_cast<unsigned long>(jpegSequence),
                  static_cast<unsigned long>(jpeg.size),
                  static_cast<unsigned long>(encodeUs),
                  validJpeg ? "yes" : "no");

#ifndef EXCLUDE_WIFI
    if (validJpeg) {
      uint32_t writeStartUs = micros();
      bool sent = sendJpegFrame(jpeg, jpegSequence, width, height);
      uint32_t writeUs = micros() - writeStartUs;
      Serial.printf("tcp sequence=%lu bytes=%lu write_us=%lu sent=%s\n",
                    static_cast<unsigned long>(jpegSequence),
                    static_cast<unsigned long>(jpeg.size),
                    static_cast<unsigned long>(writeUs),
                    sent ? "yes" : "no");
    }
#endif
  }

  if (frameCount == 1 || frameCount - lastLogFrame >= kFrameLogInterval) {
    uint32_t elapsed = now - lastLogMillis;
    float fps = elapsed > 0
              ? (1000.0f * (frameCount - lastLogFrame)) / elapsed
              : 0.0f;

    Serial.printf(
      "frame=%lu data=%p bytes=%lu expected_min=%lu valid=%s format=%s "
      "width=%lu height=%lu sample_hash=0x%08lx fps=%.2f invalid=%lu\n",
      static_cast<unsigned long>(frameCount),
      frame.data(),
      static_cast<unsigned long>(payloadSize),
      static_cast<unsigned long>(expectedRgb565Size),
      validSize ? "yes" : "no",
      frame.formatName(),
      static_cast<unsigned long>(width),
      static_cast<unsigned long>(height),
      static_cast<unsigned long>(sampleHash),
      fps,
      static_cast<unsigned long>(invalidFrameCount));

    lastLogFrame = frameCount;
    lastLogMillis = now;
  }
}
#else
void setup() {
  Serial.begin(115200);
  Serial.println("ESP_IDF_VERSION < 5.4.0, this example is not supported");
}

void loop() {
  delay(1000);
}
#endif  // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
