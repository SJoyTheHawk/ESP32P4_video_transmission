#pragma once

#include <Arduino.h>
#include <driver/jpeg_encode.h>

struct JpegEncodeResult {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
};

class JpegEncoderClass {
public:
  bool begin(uint32_t width, uint32_t height, uint32_t quality);
  bool encode(const uint8_t *rgb565, uint32_t size, JpegEncodeResult *result);

private:
  jpeg_encoder_handle_t handle_ = nullptr;
  uint8_t *output_ = nullptr;
  size_t output_capacity_ = 0;
  jpeg_encode_cfg_t config_ = {};
};
