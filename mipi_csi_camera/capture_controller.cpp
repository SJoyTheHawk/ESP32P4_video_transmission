#include "capture_controller.h"
#include "ov5647_1080p_mode.h"
#include "ov5647_800x800_mode.h"
#include "ov5647_800x640_mode.h"
#include "ov5647_800x1280_mode.h"

#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_video_ioctl.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr uint32_t kHighResWidth = 1920;
constexpr uint32_t kHighResHeight = 1080;
constexpr uint32_t kHighResJpegQuality = 90;
constexpr size_t kHighResBufferBytes =
  static_cast<size_t>(kHighResWidth) * kHighResHeight * 2;
constexpr size_t kHighResBufferCount = 2;
constexpr size_t kHighResMaxJpegBytes = 2 * 1024 * 1024;
constexpr size_t kHighResDriverReserveBytes = 1024 * 1024;
constexpr size_t kTransactionReserveBytes = 1024 * 1024;
constexpr uint32_t kHighResSettlingFrames = 3;
constexpr uint32_t kBaselineSettlingFrames = 3;

bool isSofMarker(uint8_t marker) {
  return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4
         && marker != 0xc8 && marker != 0xcc;
}
}  // namespace

CaptureController::BaselineFrame::BaselineFrame(
  CaptureController *owner, uint8_t *data, size_t size, uint32_t width,
  uint32_t height, uint32_t index, bool operation_locked)
  : owner_(owner), data_(data), size_(size), width_(width), height_(height),
    index_(index), operation_locked_(operation_locked) {}

CaptureController::BaselineFrame::BaselineFrame(BaselineFrame &&other) noexcept
  : owner_(other.owner_), data_(other.data_), size_(other.size_),
    width_(other.width_), height_(other.height_), index_(other.index_),
    operation_locked_(other.operation_locked_) {
  other.owner_ = nullptr;
  other.data_ = nullptr;
  other.size_ = 0;
  other.operation_locked_ = false;
}

CaptureController::BaselineFrame &CaptureController::BaselineFrame::operator=(
  BaselineFrame &&other) noexcept {
  if (this != &other) {
    end();
    owner_ = other.owner_;
    data_ = other.data_;
    size_ = other.size_;
    width_ = other.width_;
    height_ = other.height_;
    index_ = other.index_;
    operation_locked_ = other.operation_locked_;
    other.owner_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
    other.operation_locked_ = false;
  }
  return *this;
}

CaptureController::BaselineFrame::~BaselineFrame() { end(); }

bool CaptureController::BaselineFrame::valid() const {
  return owner_ != nullptr && data_ != nullptr && size_ > 0;
}

uint8_t *CaptureController::BaselineFrame::data() const { return data_; }
size_t CaptureController::BaselineFrame::size() const { return size_; }
uint32_t CaptureController::BaselineFrame::width() const { return width_; }
uint32_t CaptureController::BaselineFrame::height() const { return height_; }

void CaptureController::BaselineFrame::end() {
  CaptureController *owner = owner_;
  const bool operation_locked = operation_locked_;
  if (owner_ != nullptr && data_ != nullptr) {
    owner_->releaseBuffer(index_);
  }
  owner_ = nullptr;
  data_ = nullptr;
  size_ = 0;
  operation_locked_ = false;
  if (operation_locked && owner != nullptr && owner->operation_mutex_ != nullptr) {
    xSemaphoreGive(owner->operation_mutex_);
  }
}

CaptureController::CaptureController()
  : operation_mutex_(xSemaphoreCreateMutex()) {}

CaptureController::~CaptureController() {
  end();
  if (operation_mutex_ != nullptr) {
    vSemaphoreDelete(operation_mutex_);
    operation_mutex_ = nullptr;
  }
}

bool CaptureController::beginBaseline(const char *device_path,
                                      size_t buffer_count,
                                      uint32_t jpeg_quality,
                                      uint32_t timeout_ms) {
  if (device_path == nullptr || buffer_count == 0 || buffer_count > kMaxBuffers) {
    return false;
  }
  end();
  baseline_sensor_format_saved_ = false;
  device_path_ = device_path;
  requested_buffer_count_ = buffer_count;
  jpeg_quality_ = jpeg_quality;
  timeout_ms_ = timeout_ms;
  if (!openAndConfigure(true) || !requestAndMapBuffers(requested_buffer_count_)
      || !startStream()) {
    end();
    setState(CaptureControllerState::Unavailable);
    return false;
  }
  if (!jpeg_encoder_.begin(width_, height_, jpeg_quality_)) {
    Serial.println("capture controller: failed to initialize JPEG encoder");
    end();
    setState(CaptureControllerState::Unavailable);
    return false;
  }
  setState(CaptureControllerState::BaselineRunning);
  return true;
}

void CaptureController::end() {
  stopStream();
  releaseBuffers();
  closeDevice();
  jpeg_encoder_.end();
  if (state_ != CaptureControllerState::Unavailable) {
    setState(CaptureControllerState::Uninitialized);
  }
}

bool CaptureController::openAndConfigure(bool log_baseline) {
  setState(CaptureControllerState::SensorConfiguring);
  fd_ = open(device_path_, O_RDWR);
  if (fd_ < 0) {
    Serial.printf("capture controller: open failed errno=%d\n", errno);
    return false;
  }

  esp_cam_sensor_format_t sensor_format = {};
  const bool sensor_ready = ioctl(fd_, VIDIOC_G_SENSOR_FMT, &sensor_format) == 0;
  struct v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const bool format_ready = ioctl(fd_, VIDIOC_G_FMT, &format) == 0;
  if (!sensor_ready || !format_ready) {
    Serial.printf("capture controller: baseline format read failed errno=%d\n", errno);
    return false;
  }

  if (!baseline_sensor_format_saved_) {
    baseline_sensor_format_ = sensor_format;
    baseline_sensor_format_saved_ = true;
  }

  if (log_baseline) {
    Serial.printf(
      "sensor baseline mode=%s output=%ux%u format=%d fps=%u xclk=%d "
      "mipi_clk=%lu lanes=%lu\n",
      sensor_format.name == nullptr ? "unnamed" : sensor_format.name,
      sensor_format.width, sensor_format.height, static_cast<int>(sensor_format.format),
      sensor_format.fps, sensor_format.xclk,
      static_cast<unsigned long>(sensor_format.mipi_info.mipi_clk),
      static_cast<unsigned long>(sensor_format.mipi_info.lane_num));
    Serial.printf(
      "video baseline width=%lu height=%lu fourcc=" V4L2_FMT_STR
      " bytes_per_line=%lu size_image=%lu\n",
      static_cast<unsigned long>(format.fmt.pix.width),
      static_cast<unsigned long>(format.fmt.pix.height),
      V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat),
      static_cast<unsigned long>(format.fmt.pix.bytesperline),
      static_cast<unsigned long>(format.fmt.pix.sizeimage));
    probeVgaOutputFormat(format);
  }

  // Match ESPVideoCaptureDevClass::setFormat(): only issue S_FMT when the
  // pixel format actually changes. Reapplying S_FMT on every restart can leave
  // the CSI stream open without a ready buffer on the precompiled driver.
  if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd_, VIDIOC_S_FMT, &format) != 0) {
      Serial.printf("capture controller: RGB565 format setup failed errno=%d\n", errno);
      return false;
    }
  }
  if (ioctl(fd_, VIDIOC_G_FMT, &format) != 0) {
    Serial.printf("capture controller: RGB565 format readback failed errno=%d\n", errno);
    return false;
  }
  width_ = format.fmt.pix.width;
  height_ = format.fmt.pix.height;
  return configureTimeout(log_baseline);
}

bool CaptureController::configureTimeout(bool log_result) {
  struct timeval requested = {};
  requested.tv_sec = timeout_ms_ / 1000;
  requested.tv_usec = (timeout_ms_ % 1000) * 1000;
  errno = 0;
  const int set_result = ioctl(fd_, VIDIOC_S_DQBUF_TIMEOUT, &requested);
  const int set_errno = errno;
  struct timeval actual = {};
  errno = 0;
  const int get_result = ioctl(fd_, VIDIOC_G_DQBUF_TIMEOUT, &actual);
  const int get_errno = errno;
  const uint64_t actual_us = static_cast<uint64_t>(actual.tv_sec) * 1000000
                             + actual.tv_usec;
  const bool ready = set_result == 0 && get_result == 0
                     && actual_us == static_cast<uint64_t>(timeout_ms_) * 1000;
  if (log_result || !ready) {
    Serial.printf(
      "capture timeout status=%s requested_ms=%lu actual_us=%llu "
      "set_result=%d set_errno=%d get_result=%d get_errno=%d "
      "driver_timeout_errno=EPERM app_timeout_errno=ETIMEDOUT\n",
      ready ? "configured" : "failed", static_cast<unsigned long>(timeout_ms_),
      actual_us, set_result, set_errno, get_result, get_errno);
  }
  return ready;
}

bool CaptureController::requestAndMapBuffers(size_t requested_count) {
  if (requested_count == 0 || requested_count > kMaxBuffers) {
    return false;
  }
  setState(CaptureControllerState::BufferAllocating);
  struct v4l2_requestbuffers request = {};
  request.count = requested_count;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd_, VIDIOC_REQBUFS, &request) != 0 || request.count == 0
      || request.count > kMaxBuffers) {
    Serial.printf("capture controller: buffer request failed errno=%d count=%u\n",
                  errno, request.count);
    return false;
  }
  buffer_count_ = request.count;
  for (size_t i = 0; i < buffer_count_; ++i) {
    struct v4l2_buffer buffer = {};
    buffer.type = request.type;
    buffer.memory = request.memory;
    buffer.index = i;
    if (ioctl(fd_, VIDIOC_QUERYBUF, &buffer) != 0) {
      Serial.printf("capture controller: buffer query failed index=%u errno=%d\n",
                    static_cast<unsigned>(i), errno);
      return false;
    }
    void *mapping = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, buffer.m.offset);
    if (mapping == MAP_FAILED) {
      Serial.printf("capture controller: buffer map failed index=%u errno=%d\n",
                    static_cast<unsigned>(i), errno);
      return false;
    }
    buffer_ptr_[i] = static_cast<uint8_t *>(mapping);
    buffer_len_[i] = buffer.length;
  }
  return true;
}

bool CaptureController::startStream() {
  for (size_t i = 0; i < buffer_count_; ++i) {
    if (!releaseBuffer(i)) {
      return false;
    }
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd_, VIDIOC_STREAMON, &type) != 0) {
    Serial.printf("capture controller: stream start failed errno=%d\n", errno);
    return false;
  }
  stream_started_ = true;
  // Give the sensor and driver time to stabilize after STREAMON before the first
  // frame dequeue. The CSI/MIPI pipeline needs time to sync and produce frames.
  delay(100);
  return true;
}

bool CaptureController::stopStream() {
  if (!stream_started_ || fd_ < 0) {
    return true;
  }
  setState(CaptureControllerState::Stopping);
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const bool stopped = ioctl(fd_, VIDIOC_STREAMOFF, &type) == 0;
  if (!stopped) {
    Serial.printf("capture controller: stream stop failed errno=%d\n", errno);
  }
  stream_started_ = false;
  return stopped;
}

void CaptureController::releaseBuffers() {
  for (size_t i = 0; i < buffer_count_; ++i) {
    if (buffer_ptr_[i] != nullptr && buffer_ptr_[i] != MAP_FAILED) {
      munmap(buffer_ptr_[i], buffer_len_[i]);
    }
    buffer_ptr_[i] = nullptr;
    buffer_len_[i] = 0;
  }
  buffer_count_ = 0;
  if (fd_ >= 0) {
    struct v4l2_requestbuffers request = {};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    ioctl(fd_, VIDIOC_REQBUFS, &request);
  }
}

void CaptureController::closeDevice() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool CaptureController::releaseBuffer(uint32_t index) {
  if (fd_ < 0 || index >= buffer_count_) {
    return false;
  }
  struct v4l2_buffer buffer = {};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  buffer.index = index;
  if (ioctl(fd_, VIDIOC_QBUF, &buffer) != 0) {
    Serial.printf("capture controller: buffer queue failed index=%u errno=%d\n",
                  static_cast<unsigned>(index), errno);
    return false;
  }
  return true;
}

CaptureController::BaselineFrame CaptureController::captureBaselineFrame() {
  if (!isBaselineRunning()) {
    return BaselineFrame();
  }
  if (operation_mutex_ == nullptr
      || xSemaphoreTake(operation_mutex_, 0) != pdTRUE) {
    return BaselineFrame();
  }
  BaselineFrame frame = dequeueFrame();
  if (!frame.valid()) {
    xSemaphoreGive(operation_mutex_);
    return BaselineFrame();
  }
  frame.operation_locked_ = true;
  return frame;
}

CaptureController::BaselineFrame CaptureController::dequeueFrame() {
  if (!stream_started_ || fd_ < 0) {
    return BaselineFrame();
  }
  struct v4l2_buffer buffer = {};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd_, VIDIOC_DQBUF, &buffer) != 0 || buffer.index >= buffer_count_) {
    return BaselineFrame();
  }
  const size_t size = buffer.bytesused == 0 ? buffer_len_[buffer.index]
                                             : buffer.bytesused;
  return BaselineFrame(this, buffer_ptr_[buffer.index], size, width_, height_,
                       buffer.index, false);
}

bool CaptureController::encodeBaselineFrame(const BaselineFrame &frame,
                                            JpegEncodeResult *out) {
  return frame.valid() && jpeg_encoder_.encode(frame.data(), frame.size(), out);
}

bool CaptureController::runTimeoutRecoveryTest() {
  BaselineFrame held[kMaxBuffers];
  size_t held_count = 0;
  uint32_t timeout_elapsed_us = 0;
  int driver_errno = 0;
  bool timeout_observed = false;
  while (held_count < buffer_count_) {
    const uint32_t start_us = micros();
    errno = 0;
    BaselineFrame candidate = dequeueFrame();
    const uint32_t elapsed_us = micros() - start_us;
    if (!candidate.valid()) {
      if (held_count == 0) {
        Serial.printf("capture timeout test status=failed stage=hold-buffer index=0 errno=%d\n", errno);
        return false;
      }
      timeout_elapsed_us = elapsed_us;
      driver_errno = errno;
      timeout_observed = true;
      break;
    }
    held[held_count++] = static_cast<BaselineFrame &&>(candidate);
  }
  if (!timeout_observed) {
    const uint32_t start_us = micros();
    errno = 0;
    BaselineFrame timed_out = dequeueFrame();
    timeout_elapsed_us = micros() - start_us;
    driver_errno = errno;
    timeout_observed = !timed_out.valid();
  }
  const bool bounded = timeout_observed && timeout_elapsed_us >= 4500000U
                       && timeout_elapsed_us <= 6500000U;
  const bool expected_errno = driver_errno == EPERM || driver_errno == ETIMEDOUT;
  for (size_t i = 0; i < held_count; ++i) {
    held[i].end();
  }
  BaselineFrame recovery = dequeueFrame();
  const bool recovered = recovery.valid();
  recovery.end();
  const bool ready = bounded && expected_errno && recovered;
  Serial.printf(
    "capture timeout test status=%s injection=hold-available-buffers "
    "requested_ms=%lu elapsed_ms=%llu held_buffers=%u driver_errno=%d reported_errno=%d "
    "stream_started=%s recovery_frame=%s\n",
    ready ? "passed" : "failed", static_cast<unsigned long>(timeout_ms_),
    static_cast<unsigned long long>(timeout_elapsed_us / 1000),
    static_cast<unsigned>(held_count), driver_errno,
    (bounded && expected_errno) ? ETIMEDOUT : driver_errno,
    stream_started_ ? "yes" : "no", recovered ? "valid" : "invalid");
  return ready;
}

bool CaptureController::restartBaseline() {
  setState(CaptureControllerState::Restoring);
  stopStream();
  releaseBuffers();
  closeDevice();
  if (!openAndConfigure(false) || !requestAndMapBuffers(requested_buffer_count_)
      || !startStream()) {
    setState(CaptureControllerState::Unavailable);
    return false;
  }
  setState(CaptureControllerState::BaselineRunning);
  return true;
}

bool CaptureController::applySensorFormat(
  const esp_cam_sensor_format_t &sensor_format, bool log_result) {
  setState(CaptureControllerState::SensorConfiguring);
  if (fd_ < 0 || ioctl(fd_, VIDIOC_S_SENSOR_FMT, &sensor_format) != 0) {
    Serial.printf("capture controller: sensor format setup failed errno=%d\n", errno);
    return false;
  }
  if (!setRgb565Output() || !configureTimeout(false)) {
    return false;
  }
  if (log_result) {
    Serial.printf(
      "high-res sensor mode=%s output=%lux%lu format=%s fps=%u buffers=%u\n",
      sensor_format.name == nullptr ? "unnamed" : sensor_format.name,
      static_cast<unsigned long>(width_), static_cast<unsigned long>(height_),
      formatName(), sensor_format.fps, static_cast<unsigned>(kHighResBufferCount));
  }
  return true;
}

bool CaptureController::setRgb565Output() {
  struct v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd_, VIDIOC_G_FMT, &format) != 0) {
    Serial.printf("capture controller: output format read failed errno=%d\n", errno);
    return false;
  }
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(fd_, VIDIOC_S_FMT, &format) != 0
      || ioctl(fd_, VIDIOC_G_FMT, &format) != 0) {
    Serial.printf("capture controller: RGB565 format setup failed errno=%d\n", errno);
    return false;
  }
  width_ = format.fmt.pix.width;
  height_ = format.fmt.pix.height;
  return true;
}

bool CaptureController::discardSettlingFrames(uint32_t count) {
  for (uint32_t frame_number = 0; frame_number < count; ++frame_number) {
    BaselineFrame frame = dequeueFrame();
    if (!frame.valid()) {
      return false;
    }
    frame.end();
  }
  return true;
}

bool CaptureController::memoryGateFor1080p() const {
  const size_t required = kHighResBufferBytes * kHighResBufferCount
                          + kHighResBufferBytes
                          + kHighResDriverReserveBytes + kTransactionReserveBytes;
  const size_t free_psram = ESP.getFreePsram();
  const size_t largest_psram =
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const bool ready = free_psram >= required && largest_psram >= kHighResBufferBytes;
  Serial.printf(
    "high-res memory gate status=%s required=%lu psram_free=%lu psram_largest=%lu\n",
    ready ? "passed" : "failed", static_cast<unsigned long>(required),
    static_cast<unsigned long>(free_psram), static_cast<unsigned long>(largest_psram));
  return ready;
}

bool CaptureController::jpegMatchesDimensions(const JpegEncodeResult &jpeg,
                                               uint32_t width,
                                               uint32_t height) const {
  if (jpeg.data == nullptr || jpeg.size < 4 || jpeg.size > kHighResMaxJpegBytes
      || jpeg.data[0] != 0xff || jpeg.data[1] != 0xd8
      || jpeg.data[jpeg.size - 2] != 0xff || jpeg.data[jpeg.size - 1] != 0xd9) {
    return false;
  }
  size_t offset = 2;
  while (offset + 3 < jpeg.size) {
    if (jpeg.data[offset++] != 0xff) {
      return false;
    }
    while (offset < jpeg.size && jpeg.data[offset] == 0xff) {
      ++offset;
    }
    if (offset >= jpeg.size) {
      return false;
    }
    const uint8_t marker = jpeg.data[offset++];
    if (marker == 0xd9 || marker == 0xda) {
      return false;
    }
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      continue;
    }
    if (offset + 1 >= jpeg.size) {
      return false;
    }
    const size_t segment_size =
      (static_cast<size_t>(jpeg.data[offset]) << 8) | jpeg.data[offset + 1];
    if (segment_size < 2 || offset + segment_size > jpeg.size) {
      return false;
    }
    if (isSofMarker(marker)) {
      if (segment_size < 8) {
        return false;
      }
      const uint32_t jpeg_height =
        (static_cast<uint32_t>(jpeg.data[offset + 3]) << 8) | jpeg.data[offset + 4];
      const uint32_t jpeg_width =
        (static_cast<uint32_t>(jpeg.data[offset + 5]) << 8) | jpeg.data[offset + 6];
      return jpeg_width == width && jpeg_height == height;
    }
    offset += segment_size;
  }
  return false;
}

bool CaptureController::restoreBaseline() {
  setState(CaptureControllerState::Restoring);
  stopStream();
  releaseBuffers();
  if (!baseline_sensor_format_saved_
      || !applySensorFormat(baseline_sensor_format_, false)
      || !requestAndMapBuffers(requested_buffer_count_)
      || !jpeg_encoder_.begin(width_, height_, jpeg_quality_)
      || !startStream()) {
    end();
    setState(CaptureControllerState::Unavailable);
    return false;
  }
  setState(CaptureControllerState::BaselineRunning);
  if (!discardSettlingFrames(kBaselineSettlingFrames)) {
    end();
    setState(CaptureControllerState::Unavailable);
    return false;
  }
  return true;
}

bool CaptureController::capture1080pStill(HighResStillCandidate *candidate) {
  if (candidate == nullptr || !isBaselineRunning()) {
    return false;
  }
  candidate->release();
  if (operation_mutex_ == nullptr
      || xSemaphoreTake(operation_mutex_, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (!memoryGateFor1080p()) {
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  stopStream();
  releaseBuffers();
  jpeg_encoder_.end();
  bool ready = applySensorFormat(ov5647_1080p_sensor_format(), true)
               && width_ == kHighResWidth && height_ == kHighResHeight
               && requestAndMapBuffers(kHighResBufferCount) && startStream();
  if (ready) {
    setState(CaptureControllerState::HighResReady);
    ready = discardSettlingFrames(kHighResSettlingFrames);
  }

  JpegEncoderClass high_res_encoder;
  JpegEncodeResult encoded;
  if (ready) {
    setState(CaptureControllerState::Capturing);
    BaselineFrame frame = dequeueFrame();
    ready = frame.valid() && frame.size() >= kHighResBufferBytes
            && high_res_encoder.begin(width_, height_, kHighResJpegQuality)
            && high_res_encoder.encode(frame.data(), kHighResBufferBytes, &encoded)
            && jpegMatchesDimensions(encoded, width_, height_);
    frame.end();
  }
  if (ready) {
    candidate->jpeg = high_res_encoder.detachOutput();
    candidate->size = encoded.size;
    candidate->width = width_;
    candidate->height = height_;
    candidate->quality = kHighResJpegQuality;
    candidate->captured_ms = millis();
    ready = candidate->valid();
  }
  high_res_encoder.end();
  const bool restored = restoreBaseline();
  if (!ready || !restored) {
    candidate->release();
    xSemaphoreGive(operation_mutex_);
    return false;
  }
  xSemaphoreGive(operation_mutex_);
  return true;
}

bool CaptureController::run1080pCaptureValidation(uint32_t cycles) {
  if (!isBaselineRunning() || cycles == 0) {
    return false;
  }
  bool ready = true;
  uint32_t completed = 0;
  for (; completed < cycles && ready; ++completed) {
    HighResStillCandidate candidate;
    ready = capture1080pStill(&candidate);
    candidate.release();
  }
  Serial.printf(
    "high-res capture validation status=%s completed=%lu requested=%lu state=%s\n",
    ready && completed == cycles ? "passed" : "failed",
    static_cast<unsigned long>(completed), static_cast<unsigned long>(cycles),
    stateName());
  return ready && completed == cycles;
}

bool CaptureController::runRestartValidation(uint32_t cycles) {
  // The first full release/remap causes a one-time PSRAM free-list reshaping in
  // the precompiled ESP-Video allocator. Warm it up, then check that repeated
  // controller restarts remain stable from that settled baseline.
  bool warmup_ready = restartBaseline();
  if (warmup_ready) {
    BaselineFrame frame;
    for (uint32_t attempt = 1; attempt <= 3 && !frame.valid(); ++attempt) {
      errno = 0;
      frame = captureBaselineFrame();
      if (!frame.valid()) {
        delay(50);
      }
    }
    warmup_ready = frame.valid();
    frame.end();
  }
  if (!warmup_ready) {
    Serial.printf("capture restart validation status=failed warmup=failed state=%s\n",
                  stateName());
    return false;
  }

  const size_t heap_before = ESP.getFreeHeap();
  const size_t heap_largest_before = ESP.getMaxAllocHeap();
  const size_t psram_before = ESP.getFreePsram();
  const size_t psram_largest_before =
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  bool ready = cycles > 0;
  for (uint32_t cycle = 0; cycle < cycles && ready; ++cycle) {
    ready = restartBaseline();
    if (!ready) {
      break;
    }
    BaselineFrame frame;
    for (uint32_t attempt = 1; attempt <= 3 && !frame.valid(); ++attempt) {
      errno = 0;
      frame = captureBaselineFrame();
      if (!frame.valid()) {
        delay(50);
      }
    }
    ready = frame.valid();
    frame.end();
  }
  const size_t heap_after = ESP.getFreeHeap();
  const size_t heap_largest_after = ESP.getMaxAllocHeap();
  const size_t psram_after = ESP.getFreePsram();
  const size_t psram_largest_after =
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const bool memory_ready = heap_after >= heap_before
                            && heap_largest_after >= heap_largest_before
                            && psram_after >= psram_before
                            && psram_largest_after >= psram_largest_before;
  ready = ready && memory_ready;
  Serial.printf(
    "capture restart validation status=%s warmup=passed cycles=%lu state=%s "
    "heap_before=%lu heap_after=%lu heap_largest_before=%lu heap_largest_after=%lu "
    "psram_before=%lu psram_after=%lu psram_largest_before=%lu psram_largest_after=%lu\n",
    ready ? "passed" : "failed", static_cast<unsigned long>(cycles), stateName(),
    static_cast<unsigned long>(heap_before), static_cast<unsigned long>(heap_after),
    static_cast<unsigned long>(heap_largest_before),
    static_cast<unsigned long>(heap_largest_after),
    static_cast<unsigned long>(psram_before), static_cast<unsigned long>(psram_after),
    static_cast<unsigned long>(psram_largest_before),
    static_cast<unsigned long>(psram_largest_after));
  return ready;
}

bool CaptureController::probeVgaOutputFormat(const struct v4l2_format &original) {
  if (original.fmt.pix.width == 640 && original.fmt.pix.height == 480) {
    Serial.println("vga probe status=supported path=active-sensor-mode output=640x480");
    return true;
  }
  struct v4l2_format requested = original;
  requested.fmt.pix.width = 640;
  requested.fmt.pix.height = 480;
  errno = 0;
  const int set_result = ioctl(fd_, VIDIOC_S_FMT, &requested);
  const int set_errno = errno;
  struct v4l2_format actual = {};
  actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  const bool readback = ioctl(fd_, VIDIOC_G_FMT, &actual) == 0;
  const bool supported = set_result == 0 && readback
                         && actual.fmt.pix.width == 640 && actual.fmt.pix.height == 480;
  Serial.printf("vga probe status=%s requested=640x480 actual=%lux%lu set_result=%d errno=%d next=%s\n",
                supported ? "supported" : "unsupported",
                static_cast<unsigned long>(readback ? actual.fmt.pix.width : 0),
                static_cast<unsigned long>(readback ? actual.fmt.pix.height : 0),
                set_result, set_errno, supported ? "validate-hardware-output" : "add-sensor-mode");
  if (set_result == 0 && ioctl(fd_, VIDIOC_S_FMT, &original) != 0) {
    Serial.printf("vga probe restore=failed errno=%d\n", errno);
    return false;
  }
  return supported;
}

void CaptureController::setState(CaptureControllerState state) { state_ = state; }

bool CaptureController::isBaselineRunning() const {
  return state_ == CaptureControllerState::BaselineRunning && stream_started_;
}

CaptureControllerState CaptureController::state() const { return state_; }

const char *CaptureController::stateName() const {
  switch (state_) {
    case CaptureControllerState::Uninitialized: return "Uninitialized";
    case CaptureControllerState::BaselineRunning: return "BaselineRunning";
    case CaptureControllerState::Stopping: return "Stopping";
    case CaptureControllerState::SensorConfiguring: return "SensorConfiguring";
    case CaptureControllerState::BufferAllocating: return "BufferAllocating";
    case CaptureControllerState::HighResReady: return "HighResReady";
    case CaptureControllerState::Capturing: return "Capturing";
    case CaptureControllerState::Restoring: return "Restoring";
    case CaptureControllerState::Unavailable: return "Unavailable";
  }
  return "Unknown";
}

uint32_t CaptureController::width() const { return width_; }
uint32_t CaptureController::height() const { return height_; }
const char *CaptureController::formatName() const { return "RGB565"; }

bool CaptureController::switchResolution(StreamResolution target) {
  if (xSemaphoreTake(operation_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.printf("resolution switch: blocked by operation lock\n");
    return false;
  }

  if (state_ != CaptureControllerState::BaselineRunning) {
    Serial.printf("resolution switch: invalid state=%s\n", stateName());
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  if (target == current_resolution_) {
    Serial.printf("resolution switch: already at target=%s\n", resolutionName(target));
    xSemaphoreGive(operation_mutex_);
    return true;
  }

  const esp_cam_sensor_format_t* target_format = getSensorFormatForResolution(target);
  if (target_format == nullptr) {
    Serial.printf("resolution switch: unsupported target=%d\n", static_cast<int>(target));
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  if (!memoryGateForResolution(target)) {
    Serial.printf("resolution switch: insufficient memory for target=%s\n",
                  resolutionName(target));
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Save current state for rollback
  StreamResolution previous_resolution = current_resolution_;
  esp_cam_sensor_format_t previous_format = baseline_sensor_format_;
  uint32_t previous_width = width_;
  uint32_t previous_height = height_;

  Serial.printf("resolution switch: from=%s (%ux%u) to=%s (%ux%u)\n",
                resolutionName(previous_resolution), previous_width, previous_height,
                resolutionName(target), target_format->width, target_format->height);

  // Stop current stream
  stopStream();
  releaseBuffers();
  jpeg_encoder_.end();

  // Apply new sensor format
  setState(CaptureControllerState::SensorConfiguring);
  errno = 0;
  if (ioctl(fd_, VIDIOC_S_SENSOR_FMT, target_format) != 0) {
    Serial.printf("resolution switch: sensor format failed errno=%d\n", errno);
    // Rollback
    ioctl(fd_, VIDIOC_S_SENSOR_FMT, &previous_format);
    setRgb565Output();
    width_ = previous_width;
    height_ = previous_height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(width_, height_, jpeg_quality_);
    startStream();
    setState(CaptureControllerState::BaselineRunning);
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Set RGB565 output
  if (!setRgb565Output()) {
    Serial.printf("resolution switch: RGB565 format failed\n");
    // Rollback
    ioctl(fd_, VIDIOC_S_SENSOR_FMT, &previous_format);
    setRgb565Output();
    width_ = previous_width;
    height_ = previous_height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(width_, height_, jpeg_quality_);
    startStream();
    setState(CaptureControllerState::BaselineRunning);
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Update dimensions
  width_ = target_format->width;
  height_ = target_format->height;
  baseline_sensor_format_ = *target_format;

  // Allocate new buffers
  if (!requestAndMapBuffers(requested_buffer_count_)) {
    Serial.printf("resolution switch: buffer allocation failed\n");
    // Rollback
    ioctl(fd_, VIDIOC_S_SENSOR_FMT, &previous_format);
    setRgb565Output();
    width_ = previous_width;
    height_ = previous_height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_width, previous_height, jpeg_quality_);
    startStream();
    setState(CaptureControllerState::BaselineRunning);
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Reinitialize JPEG encoder for new dimensions
  if (!jpeg_encoder_.begin(width_, height_, jpeg_quality_)) {
    Serial.printf("resolution switch: JPEG encoder init failed\n");
    // Rollback
    releaseBuffers();
    ioctl(fd_, VIDIOC_S_SENSOR_FMT, &previous_format);
    setRgb565Output();
    width_ = previous_width;
    height_ = previous_height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_width, previous_height, jpeg_quality_);
    startStream();
    setState(CaptureControllerState::BaselineRunning);
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Start new stream
  if (!startStream()) {
    Serial.printf("resolution switch: stream start failed\n");
    // Rollback
    jpeg_encoder_.end();
    releaseBuffers();
    ioctl(fd_, VIDIOC_S_SENSOR_FMT, &previous_format);
    setRgb565Output();
    width_ = previous_width;
    height_ = previous_height;
    baseline_sensor_format_ = previous_format;
    requestAndMapBuffers(requested_buffer_count_);
    jpeg_encoder_.begin(previous_width, previous_height, jpeg_quality_);
    startStream();
    setState(CaptureControllerState::BaselineRunning);
    xSemaphoreGive(operation_mutex_);
    return false;
  }

  // Discard settling frames
  discardSettlingFrames(kBaselineSettlingFrames);

  // Success
  current_resolution_ = target;
  setState(CaptureControllerState::BaselineRunning);

  Serial.printf("resolution switch: success new_resolution=%s (%ux%u)\n",
                resolutionName(current_resolution_), width_, height_);

  xSemaphoreGive(operation_mutex_);
  return true;
}

StreamResolution CaptureController::getCurrentResolution() const {
  return current_resolution_;
}

bool CaptureController::isResolutionSupported(StreamResolution resolution) const {
  return getSensorFormatForResolution(resolution) != nullptr;
}

const char *CaptureController::resolutionName(StreamResolution resolution) const {
  switch (resolution) {
    case StreamResolution::XVGA_800x800: return "XVGA";
    case StreamResolution::WVGA_800x640: return "WVGA";
    case StreamResolution::Portrait_800x1280: return "Portrait";
    default: return "UNKNOWN";
  }
}

const esp_cam_sensor_format_t* CaptureController::getSensorFormatForResolution(
    StreamResolution resolution) const {
  switch (resolution) {
    case StreamResolution::XVGA_800x800:
      return &ov5647_800x800_sensor_format();
    case StreamResolution::WVGA_800x640:
      return &ov5647_800x640_sensor_format();
    case StreamResolution::Portrait_800x1280:
      return &ov5647_800x1280_sensor_format();
    default:
      return nullptr;
  }
}

bool CaptureController::memoryGateForResolution(StreamResolution resolution) const {
  const esp_cam_sensor_format_t* format = getSensorFormatForResolution(resolution);
  if (format == nullptr) {
    return false;
  }

  const size_t buffer_bytes = static_cast<size_t>(format->width) * format->height * 2;
  const size_t total_buffers = buffer_bytes * requested_buffer_count_;
  const size_t jpeg_encoder_estimate = 300 * 1024;
  const size_t reserve = 512 * 1024;
  const size_t required = total_buffers + jpeg_encoder_estimate + reserve;

  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  const bool fits = psram_free >= required && psram_largest >= buffer_bytes;

  if (!fits) {
    Serial.printf(
      "resolution gate: failed resolution=%s required=%u psram_free=%u psram_largest=%u\n",
      resolutionName(resolution), required, psram_free, psram_largest);
  }

  return fits;
}
