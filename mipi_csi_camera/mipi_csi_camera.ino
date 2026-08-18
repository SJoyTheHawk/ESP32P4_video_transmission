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
#include <esp_video_ioctl.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "jpeg_encoder.h"
#include "jpeg_output_buffer.h"

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
ESPVideoCaptureDevClass capture_dev;
JpegEncoderClass jpeg_encoder;

// Keep this identifier stable within a phase so serial logs can be matched to
// the implementation and build format that produced them.
static constexpr char kImplementationVersion[] = "v3.0-phase2-arduino";
const size_t kCaptureBufferCount = 2;
const uint32_t kJpegQuality = 50;
const uint32_t kJpegIntervalMs = 100;
const uint32_t kBaselineReportIntervalMs = 5000;
const uint32_t kCaptureDequeueTimeoutMs = 5000;
const uint32_t kLifecycleTestCycles = 100;
const size_t kStillJpegCapacity = 2 * 1024 * 1024;
const uint32_t kVgaWidth = 640;
const uint32_t kVgaHeight = 480;

struct MemorySnapshot {
  size_t heap_free;
  size_t heap_largest;
  size_t psram_free;
  size_t psram_largest;
};

static MemorySnapshot captureMemorySnapshot() {
  return {
    ESP.getFreeHeap(),
    ESP.getMaxAllocHeap(),
    ESP.getFreePsram(),
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
  };
}

static void logMemoryBaseline(const char *milestone) {
  Serial.printf(
    "memory milestone=%s heap_free=%lu heap_largest=%lu "
    "psram_total=%lu psram_free=%lu psram_largest=%lu\n",
    milestone,
    static_cast<unsigned long>(ESP.getFreeHeap()),
    static_cast<unsigned long>(ESP.getMaxAllocHeap()),
    static_cast<unsigned long>(ESP.getPsramSize()),
    static_cast<unsigned long>(ESP.getFreePsram()),
    static_cast<unsigned long>(
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

static bool logSensorIdentity() {
#if CONFIG_CAMERA_OV5647 && \
    CONFIG_CAMERA_OV5647_AUTO_DETECT_MIPI_INTERFACE_SENSOR
  if (!video.isCSIInitialized()) {
    Serial.println("sensor identity status=failed reason=csi-not-initialized");
    return false;
  }

  // video.begin() succeeds only after the OV5647 driver reads and validates
  // chip-ID registers 0x300a and 0x300b.
  Serial.println(
    "sensor identity status=confirmed pid=0x5647 "
    "source=esp-video-register-probe");
  return true;
#else
  Serial.println(
    "sensor identity status=unverified reason=ov5647-autodetect-disabled");
  return false;
#endif
}

static bool probeVgaOutputFormat(int fd,
                                 const struct v4l2_format &original_format) {
  if (original_format.fmt.pix.width == kVgaWidth
      && original_format.fmt.pix.height == kVgaHeight) {
    Serial.println(
      "vga probe status=supported path=active-sensor-mode output=640x480");
    return true;
  }

  struct v4l2_format requested_format = original_format;
  requested_format.fmt.pix.width = kVgaWidth;
  requested_format.fmt.pix.height = kVgaHeight;
  errno = 0;
  const int set_result = ioctl(fd, VIDIOC_S_FMT, &requested_format);
  const int set_errno = errno;

  struct v4l2_format actual_format = {};
  actual_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const bool readback_ready = ioctl(fd, VIDIOC_G_FMT, &actual_format) == 0;
  const bool supported = set_result == 0 && readback_ready
                         && actual_format.fmt.pix.width == kVgaWidth
                         && actual_format.fmt.pix.height == kVgaHeight;

  Serial.printf(
    "vga probe status=%s requested=%lux%lu actual=%lux%lu "
    "set_result=%d errno=%d next=%s\n",
    supported ? "supported" : "unsupported",
    static_cast<unsigned long>(kVgaWidth),
    static_cast<unsigned long>(kVgaHeight),
    static_cast<unsigned long>(
      readback_ready ? actual_format.fmt.pix.width : 0),
    static_cast<unsigned long>(
      readback_ready ? actual_format.fmt.pix.height : 0),
    set_result, set_errno,
    supported ? "validate-hardware-output" : "add-sensor-mode");

  if (set_result == 0) {
    struct v4l2_format restore_format = original_format;
    if (ioctl(fd, VIDIOC_S_FMT, &restore_format) != 0) {
      Serial.printf("vga probe restore=failed errno=%d\n", errno);
      return false;
    }
    Serial.println("vga probe restore=ready");
  }
  return supported;
}

static bool configureCaptureDequeueTimeout(int fd) {
  struct timeval requested = {};
  requested.tv_sec = kCaptureDequeueTimeoutMs / 1000;
  requested.tv_usec = (kCaptureDequeueTimeoutMs % 1000) * 1000;

  errno = 0;
  const int set_result = ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &requested);
  const int set_errno = errno;

  struct timeval actual = {};
  errno = 0;
  const int get_result = ioctl(fd, VIDIOC_G_DQBUF_TIMEOUT, &actual);
  const int get_errno = errno;
  const uint64_t actual_us = static_cast<uint64_t>(actual.tv_sec) * 1000000
                             + actual.tv_usec;
  const uint64_t requested_us =
    static_cast<uint64_t>(kCaptureDequeueTimeoutMs) * 1000;
  const bool ready = set_result == 0 && get_result == 0
                     && actual_us == requested_us;

  Serial.printf(
    "capture timeout status=%s requested_ms=%lu actual_us=%llu "
    "set_result=%d set_errno=%d get_result=%d get_errno=%d "
    "driver_timeout_errno=EPERM app_timeout_errno=ETIMEDOUT\n",
    ready ? "configured" : "failed",
    static_cast<unsigned long>(kCaptureDequeueTimeoutMs), actual_us,
    set_result, set_errno, get_result, get_errno);
  return ready;
}

static bool runJpegResourceLifecycleTest() {
  bool allocation_cycles_ready = true;
  bool encoder_cycles_ready = true;
  bool detach_ready = false;

  // Warm up lazy driver allocations before taking the comparison baseline.
  {
    JpegOutputBuffer output;
    allocation_cycles_ready = output.allocate(kStillJpegCapacity);
  }
  jpeg_encode_engine_cfg_t engine_config = {};
  engine_config.timeout_ms = 40;
  jpeg_encoder_handle_t warmup_handle = nullptr;
  if (jpeg_new_encoder_engine(&engine_config, &warmup_handle) == ESP_OK) {
    encoder_cycles_ready = jpeg_del_encoder_engine(warmup_handle) == ESP_OK;
  } else {
    encoder_cycles_ready = false;
  }

  const MemorySnapshot before = captureMemorySnapshot();
  for (uint32_t cycle = 0;
       cycle < kLifecycleTestCycles && allocation_cycles_ready; ++cycle) {
    JpegOutputBuffer output;
    allocation_cycles_ready = output.allocate(kStillJpegCapacity)
                              && output.capacity() >= kStillJpegCapacity;
  }

  for (uint32_t cycle = 0;
       cycle < kLifecycleTestCycles && encoder_cycles_ready; ++cycle) {
    jpeg_encoder_handle_t handle = nullptr;
    encoder_cycles_ready =
      jpeg_new_encoder_engine(&engine_config, &handle) == ESP_OK;
    if (encoder_cycles_ready) {
      encoder_cycles_ready = jpeg_del_encoder_engine(handle) == ESP_OK;
    }
  }

  {
    JpegOutputBuffer output;
    if (output.allocate(4096)) {
      DetachedJpegOutputBuffer detached = output.detach();
      detach_ready = !output.valid() && output.capacity() == 0
                     && detached.data != nullptr && detached.capacity >= 4096;
      free(detached.data);
      output.release();
    }
  }

  const MemorySnapshot after = captureMemorySnapshot();
  const bool memory_ready = after.heap_free >= before.heap_free
                            && after.heap_largest >= before.heap_largest
                            && after.psram_free >= before.psram_free
                            && after.psram_largest >= before.psram_largest;
  const bool ready = allocation_cycles_ready && encoder_cycles_ready
                     && detach_ready && memory_ready;

  Serial.printf(
    "jpeg lifecycle status=%s cycles=%lu capacity=%lu "
    "alloc_release=%s engine_create_destroy=%s detach=%s "
    "heap_free_before=%lu heap_free_after=%lu "
    "heap_largest_before=%lu heap_largest_after=%lu "
    "psram_free_before=%lu psram_free_after=%lu "
    "psram_largest_before=%lu psram_largest_after=%lu\n",
    ready ? "passed" : "failed",
    static_cast<unsigned long>(kLifecycleTestCycles),
    static_cast<unsigned long>(kStillJpegCapacity),
    allocation_cycles_ready ? "passed" : "failed",
    encoder_cycles_ready ? "passed" : "failed",
    detach_ready ? "passed" : "failed",
    static_cast<unsigned long>(before.heap_free),
    static_cast<unsigned long>(after.heap_free),
    static_cast<unsigned long>(before.heap_largest),
    static_cast<unsigned long>(after.heap_largest),
    static_cast<unsigned long>(before.psram_free),
    static_cast<unsigned long>(after.psram_free),
    static_cast<unsigned long>(before.psram_largest),
    static_cast<unsigned long>(after.psram_largest));
  return ready;
}

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

#ifndef EXCLUDE_WIFI
RTSPServer rtsp_server;
bool rtsp_server_ready = false;
bool firmware_ready = false;
uint32_t last_wifi_reconnect_ms = 0;
uint32_t stream_sequence = 0;
uint32_t dropped_frames = 0;
uint32_t maximum_send_us = 0;
uint32_t maximum_cycle_us = 0;
uint32_t baseline_window_start_ms = 0;
uint32_t baseline_window_frames = 0;
uint32_t baseline_window_dropped_start = 0;
uint64_t baseline_window_jpeg_bytes = 0;
uint64_t baseline_window_capture_us = 0;
uint64_t baseline_window_encode_us = 0;
uint64_t baseline_window_send_us = 0;

static void recordBaselineFrame(uint32_t jpeg_size, uint32_t capture_us,
                                uint32_t encode_us, uint32_t send_us) {
  const uint32_t now = millis();
  if (baseline_window_start_ms == 0) {
    baseline_window_start_ms = now;
    baseline_window_dropped_start = dropped_frames;
  }

  ++baseline_window_frames;
  baseline_window_jpeg_bytes += jpeg_size;
  baseline_window_capture_us += capture_us;
  baseline_window_encode_us += encode_us;
  baseline_window_send_us += send_us;

  const uint32_t elapsed_ms = now - baseline_window_start_ms;
  if (elapsed_ms < kBaselineReportIntervalMs || baseline_window_frames == 0) {
    return;
  }

  const uint32_t fps_x100 = static_cast<uint32_t>(
    (static_cast<uint64_t>(baseline_window_frames) * 100000) / elapsed_ms);
  Serial.printf(
    "baseline window_ms=%lu frames=%lu fps=%lu.%02lu "
    "avg_jpeg_bytes=%llu avg_capture_us=%llu avg_encode_us=%llu "
    "avg_send_us=%llu dropped=%lu\n",
    static_cast<unsigned long>(elapsed_ms),
    static_cast<unsigned long>(baseline_window_frames),
    static_cast<unsigned long>(fps_x100 / 100),
    static_cast<unsigned long>(fps_x100 % 100),
    baseline_window_jpeg_bytes / baseline_window_frames,
    baseline_window_capture_us / baseline_window_frames,
    baseline_window_encode_us / baseline_window_frames,
    baseline_window_send_us / baseline_window_frames,
    static_cast<unsigned long>(dropped_frames - baseline_window_dropped_start));

  baseline_window_start_ms = now;
  baseline_window_frames = 0;
  baseline_window_dropped_start = dropped_frames;
  baseline_window_jpeg_bytes = 0;
  baseline_window_capture_us = 0;
  baseline_window_encode_us = 0;
  baseline_window_send_us = 0;
}

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
  Serial.printf("implementation version=%s\n", kImplementationVersion);
  logMemoryBaseline("boot");

#ifndef EXCLUDE_WIFI
  const bool wireless_ready = initializeWirelessLink();
  Serial.printf("wireless validation: %s\n",
                wireless_ready ? "ready" : "not ready");
  if (!wireless_ready) {
    return;
  }
  logMemoryBaseline("wifi-ready");
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
  logSensorIdentity();
  if (!logCameraBaseline()) {
    Serial.println("Phase 2 gate failed: camera baseline");
    return;
  }
  logMemoryBaseline("camera-initialized");

  if (!runJpegResourceLifecycleTest()) {
    Serial.println("Phase 2 gate failed: JPEG resource lifecycle");
    return;
  }

  if (!capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
                         kCaptureBufferCount)) {
    Serial.println("failed to open capture device");
    return;
  }
  if (!capture_dev.setFormat(ESP_VIDEO_FORMAT_RGB565)) {
    Serial.println("failed to set format");
    return;
  }
  if (!configureActiveCaptureDequeueTimeout()) {
    Serial.println("Phase 2 gate failed: active dequeue timeout configuration");
    return;
  }
  Serial.printf(
    "capture baseline width=%lu height=%lu format=%s buffers=%u\n",
    static_cast<unsigned long>(capture_dev.getWidth()),
    static_cast<unsigned long>(capture_dev.getHeight()),
    capture_dev.getFormatName(), static_cast<unsigned>(kCaptureBufferCount));
  logMemoryBaseline("capture-buffers-ready");
  if (!capture_dev.startCapture()) {
    Serial.println("failed to start capture");
    return;
  }

  if (!runCaptureTimeoutRecoveryTest()) {
    Serial.println("Phase 2 gate failed: bounded dequeue timeout recovery");
    return;
  }

  if (!jpeg_encoder.begin(capture_dev.getWidth(), capture_dev.getHeight(),
                          kJpegQuality)) {
    Serial.println("failed to initialize JPEG encoder");
    return;
  }
  Serial.printf("JPEG baseline quality=%lu interval_ms=%lu target_fps=%lu\n",
                static_cast<unsigned long>(kJpegQuality),
                static_cast<unsigned long>(kJpegIntervalMs),
                static_cast<unsigned long>(1000 / kJpegIntervalMs));
  logMemoryBaseline("jpeg-encoder-ready");

#ifndef EXCLUDE_WIFI
  rtsp_server_ready = startRtspServer();
  firmware_ready = true;
#else
  Serial.println("camera ready; RTSP server excluded");
#endif
}

void loop() {
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
  ESPVideoBufferClass frame = capture_dev.captureBuffer();
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
    "rtp sequence=%lu width=%lu height=%lu bytes=%lu packets=%u "
    "capture_us=%lu encode_us=%lu "
    "send_us=%lu cycle_us=%lu max_send_us=%lu max_cycle_us=%lu "
    "sent=%s error=%d dropped=%lu\n",
    static_cast<unsigned long>(stream_sequence),
    static_cast<unsigned long>(width),
    static_cast<unsigned long>(height),
    static_cast<unsigned long>(jpeg.size), send_result.packetCount,
    static_cast<unsigned long>(capture_us),
    static_cast<unsigned long>(encode_us),
    static_cast<unsigned long>(send_us),
    static_cast<unsigned long>(cycle_us),
    static_cast<unsigned long>(maximum_send_us),
    static_cast<unsigned long>(maximum_cycle_us),
    send_result.sent ? "yes" : "no", send_result.error,
    static_cast<unsigned long>(dropped_frames));

  recordBaselineFrame(jpeg.size, capture_us, encode_us, send_us);

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
