#include "jpeg_encoder.h"

JpegEncoderClass::~JpegEncoderClass() {
  end();
}

bool JpegEncoderClass::begin(uint32_t width, uint32_t height, uint32_t quality) {
  end();

  jpeg_encode_engine_cfg_t engineConfig = {};
  engineConfig.timeout_ms = 40;
  if (jpeg_new_encoder_engine(&engineConfig, &handle_) != ESP_OK) {
    return false;
  }

  const size_t requestedSize = static_cast<size_t>(width) * height * 2;
  if (!output_.allocate(requestedSize)) {
    end();
    return false;
  }

  config_.width = width;
  config_.height = height;
  config_.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  config_.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  config_.image_quality = quality;
  return true;
}

void JpegEncoderClass::end() {
  output_.release();
  if (handle_ != nullptr) {
    jpeg_del_encoder_engine(handle_);
    handle_ = nullptr;
  }
  config_ = {};
}

bool JpegEncoderClass::encode(const uint8_t *rgb565, uint32_t size,
                              JpegEncodeResult *result) {
  if (handle_ == nullptr || !output_.valid() || result == nullptr) {
    return false;
  }

  uint32_t encodedSize = 0;
  if (jpeg_encoder_process(handle_, &config_, rgb565, size, output_.data(),
                           output_.capacity(), &encodedSize) != ESP_OK) {
    return false;
  }

  result->data = output_.data();
  result->size = encodedSize;
  return true;
}
