#pragma once

#include <Arduino.h>

struct DetachedJpegOutputBuffer {
  uint8_t *data = nullptr;
  size_t capacity = 0;
};

class JpegOutputBuffer {
public:
  JpegOutputBuffer() = default;
  ~JpegOutputBuffer();

  JpegOutputBuffer(const JpegOutputBuffer &) = delete;
  JpegOutputBuffer &operator=(const JpegOutputBuffer &) = delete;

  bool allocate(size_t requested_size);
  void release();
  DetachedJpegOutputBuffer detach();

  uint8_t *data() const;
  size_t capacity() const;
  bool valid() const;

private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
};
