#pragma once

#include "Arduino.h"
#include <ESP_Video.h>
#include <esp_cam_sensor_types.h>
#include "jpeg_encoder.h"

enum class CaptureControllerState : uint8_t {
  Uninitialized,
  BaselineRunning,
  Stopping,
  SensorConfiguring,
  BufferAllocating,
  HighResReady,
  Capturing,
  Restoring,
  Unavailable,
};

enum class StreamResolution : uint8_t {
  XVGA_800x800,
  WVGA_800x640,
  Portrait_800x1280,
};

struct HighResStillCandidate {
  DetachedJpegOutputBuffer jpeg;
  uint32_t size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t quality = 0;
  uint64_t captured_ms = 0;

  void release() {
    JpegOutputBuffer::releaseDetached(jpeg);
    jpeg = {};
    size = 0;
  }
  bool valid() const { return jpeg.data != nullptr && size > 0; }
};

class CaptureController {
public:
  CaptureController();
  ~CaptureController();

  class BaselineFrame {
  public:
    BaselineFrame() = default;
    BaselineFrame(const BaselineFrame &) = delete;
    BaselineFrame &operator=(const BaselineFrame &) = delete;
    BaselineFrame(BaselineFrame &&other) noexcept;
    BaselineFrame &operator=(BaselineFrame &&other) noexcept;
    ~BaselineFrame();

    bool valid() const;
    uint8_t *data() const;
    size_t size() const;
    uint32_t width() const;
    uint32_t height() const;
    void end();

  private:
    friend class CaptureController;
    BaselineFrame(CaptureController *owner, uint8_t *data, size_t size,
                  uint32_t width, uint32_t height, uint32_t index,
                  bool operation_locked);

    CaptureController *owner_ = nullptr;
    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t index_ = 0;
    bool operation_locked_ = false;
  };

  bool beginBaseline(const char *device_path, size_t buffer_count,
                     uint32_t jpeg_quality, uint32_t timeout_ms);
  void end();

  BaselineFrame captureBaselineFrame();
  bool encodeBaselineFrame(const BaselineFrame &frame, JpegEncodeResult *out);
  bool runTimeoutRecoveryTest();
  bool runRestartValidation(uint32_t cycles);
  bool run1080pCaptureValidation(uint32_t cycles);
  bool capture1080pStill(HighResStillCandidate *candidate);

  bool switchResolution(StreamResolution target);
  StreamResolution getCurrentResolution() const;
  bool isResolutionSupported(StreamResolution resolution) const;
  const char *resolutionName(StreamResolution resolution) const;

  bool isBaselineRunning() const;
  CaptureControllerState state() const;
  const char *stateName() const;
  uint32_t width() const;
  uint32_t height() const;
  const char *formatName() const;

private:
  static constexpr size_t kMaxBuffers = 4;

  bool openAndConfigure(bool log_baseline);
  bool configureTimeout(bool log_result);
  bool requestAndMapBuffers(size_t requested_count);
  bool startStream();
  bool stopStream();
  void releaseBuffers();
  void closeDevice();
  bool restartBaseline();
  bool applySensorFormat(const esp_cam_sensor_format_t &sensor_format,
                         bool log_result);
  bool setRgb565Output();
  bool restoreBaseline();
  bool discardSettlingFrames(uint32_t count);
  bool memoryGateFor1080p() const;
  bool jpegMatchesDimensions(const JpegEncodeResult &jpeg, uint32_t width,
                             uint32_t height) const;
  BaselineFrame dequeueFrame();
  bool releaseBuffer(uint32_t index);
  bool probeVgaOutputFormat(const struct v4l2_format &original_format);
  void setState(CaptureControllerState state);
  const esp_cam_sensor_format_t* getSensorFormatForResolution(
    StreamResolution resolution) const;
  bool memoryGateForResolution(StreamResolution resolution) const;

  int fd_ = -1;
  const char *device_path_ = nullptr;
  size_t requested_buffer_count_ = 0;
  size_t buffer_count_ = 0;
  uint8_t *buffer_ptr_[kMaxBuffers] = {};
  size_t buffer_len_[kMaxBuffers] = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t jpeg_quality_ = 0;
  uint32_t timeout_ms_ = 0;
  esp_cam_sensor_format_t baseline_sensor_format_ = {};
  bool baseline_sensor_format_saved_ = false;
  bool stream_started_ = false;
  CaptureControllerState state_ = CaptureControllerState::Uninitialized;
  StreamResolution current_resolution_ = StreamResolution::XVGA_800x800;
  JpegEncoderClass jpeg_encoder_;
  SemaphoreHandle_t operation_mutex_ = nullptr;
};
