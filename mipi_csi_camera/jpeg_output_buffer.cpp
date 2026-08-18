#include "jpeg_output_buffer.h"

#include <cstdlib>
#include <driver/jpeg_encode.h>

JpegOutputBuffer::~JpegOutputBuffer() {
  release();
}

bool JpegOutputBuffer::allocate(size_t requested_size) {
  release();
  if (requested_size == 0) {
    return false;
  }

  jpeg_encode_memory_alloc_cfg_t memory_config = {};
  memory_config.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  data_ = static_cast<uint8_t *>(
    jpeg_alloc_encoder_mem(requested_size, &memory_config, &capacity_));
  if (data_ == nullptr) {
    capacity_ = 0;
    return false;
  }
  return true;
}

void JpegOutputBuffer::release() {
  free(data_);
  data_ = nullptr;
  capacity_ = 0;
}

DetachedJpegOutputBuffer JpegOutputBuffer::detach() {
  DetachedJpegOutputBuffer detached = {data_, capacity_};
  data_ = nullptr;
  capacity_ = 0;
  return detached;
}

uint8_t *JpegOutputBuffer::data() const {
  return data_;
}

size_t JpegOutputBuffer::capacity() const {
  return capacity_;
}

bool JpegOutputBuffer::valid() const {
  return data_ != nullptr;
}
