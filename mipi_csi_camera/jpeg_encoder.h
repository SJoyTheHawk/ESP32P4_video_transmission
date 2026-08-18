#pragma once

#include <Arduino.h>
#include <driver/jpeg_encode.h>
#include "jpeg_output_buffer.h"

struct JpegEncodeResult {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
};

class JpegEncoderClass {
public:
  ~JpegEncoderClass();

  bool begin(uint32_t width, uint32_t height, uint32_t quality);
  void end();
  bool encode(const uint8_t *rgb565, uint32_t size, JpegEncodeResult *result);

private:
  jpeg_encoder_handle_t handle_ = nullptr;
  JpegOutputBuffer output_;
  jpeg_encode_cfg_t config_ = {};
};
