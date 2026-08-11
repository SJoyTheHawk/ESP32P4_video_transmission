#pragma once

#include <stddef.h>
#include <stdint.h>

struct Rfc2435JpegFrame {
  const uint8_t *scan_data = nullptr;
  size_t scan_size = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t quantization_tables[128] = {};
};

bool parseRfc2435Jpeg(const uint8_t *jpeg, size_t jpeg_size,
                      uint16_t expected_width, uint16_t expected_height,
                      Rfc2435JpegFrame *frame, const char **error);
