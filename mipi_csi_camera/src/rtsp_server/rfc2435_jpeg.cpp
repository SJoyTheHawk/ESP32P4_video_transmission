#include "rfc2435_jpeg.h"

#include <string.h>

namespace {

const uint8_t kDcLuminanceCounts[16] = {
  0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
const uint8_t kDcChrominanceCounts[16] = {
  0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
const uint8_t kDcValues[12] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
const uint8_t kAcLuminanceCounts[16] = {
  0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
const uint8_t kAcLuminanceValues[162] = {
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
  0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
  0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
  0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
  0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
  0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
  0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
  0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
  0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
  0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
  0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
  0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
  0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
  0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
  0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
  0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
  0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
  0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
  0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa
};
const uint8_t kAcChrominanceCounts[16] = {
  0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
const uint8_t kAcChrominanceValues[162] = {
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
  0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
  0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
  0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
  0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
  0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
  0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
  0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
  0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
  0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
  0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
  0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
  0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
  0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
  0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
  0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
  0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
  0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
  0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
  0xf9, 0xfa
};

uint16_t readBe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] << 8) | data[1];
}

bool fail(const char *message, const char **error) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

bool validateHuffmanTable(uint8_t table_info, const uint8_t *counts,
                          const uint8_t *values, size_t value_count,
                          uint8_t *table_mask) {
  const uint8_t *expected_counts = nullptr;
  const uint8_t *expected_values = nullptr;
  size_t expected_value_count = 0;
  uint8_t mask = 0;

  switch (table_info) {
    case 0x00:
      expected_counts = kDcLuminanceCounts;
      expected_values = kDcValues;
      expected_value_count = sizeof(kDcValues);
      mask = 0x01;
      break;
    case 0x01:
      expected_counts = kDcChrominanceCounts;
      expected_values = kDcValues;
      expected_value_count = sizeof(kDcValues);
      mask = 0x02;
      break;
    case 0x10:
      expected_counts = kAcLuminanceCounts;
      expected_values = kAcLuminanceValues;
      expected_value_count = sizeof(kAcLuminanceValues);
      mask = 0x04;
      break;
    case 0x11:
      expected_counts = kAcChrominanceCounts;
      expected_values = kAcChrominanceValues;
      expected_value_count = sizeof(kAcChrominanceValues);
      mask = 0x08;
      break;
    default:
      return false;
  }

  if (value_count != expected_value_count
      || memcmp(counts, expected_counts, 16) != 0
      || memcmp(values, expected_values, value_count) != 0) {
    return false;
  }
  *table_mask |= mask;
  return true;
}

bool parseDqt(const uint8_t *data, size_t size, Rfc2435JpegFrame *frame,
              uint8_t *table_mask) {
  size_t offset = 0;
  while (offset < size) {
    if (size - offset < 65) {
      return false;
    }
    uint8_t table_info = data[offset++];
    uint8_t precision = table_info >> 4;
    uint8_t table_id = table_info & 0x0f;
    if (precision != 0 || table_id > 1) {
      return false;
    }
    memcpy(frame->quantization_tables + table_id * 64, data + offset, 64);
    *table_mask |= static_cast<uint8_t>(1U << table_id);
    offset += 64;
  }
  return offset == size;
}

bool parseDht(const uint8_t *data, size_t size, uint8_t *table_mask) {
  size_t offset = 0;
  while (offset < size) {
    if (size - offset < 17) {
      return false;
    }
    uint8_t table_info = data[offset++];
    const uint8_t *counts = data + offset;
    size_t value_count = 0;
    for (size_t i = 0; i < 16; ++i) {
      value_count += counts[i];
    }
    offset += 16;
    if (value_count > size - offset
        || !validateHuffmanTable(table_info, counts, data + offset,
                                 value_count, table_mask)) {
      return false;
    }
    offset += value_count;
  }
  return offset == size;
}

bool parseSof0(const uint8_t *data, size_t size, Rfc2435JpegFrame *frame) {
  if (size != 15 || data[0] != 8 || data[5] != 3) {
    return false;
  }
  frame->height = readBe16(data + 1);
  frame->width = readBe16(data + 3);

  uint8_t component_mask = 0;
  for (size_t i = 0; i < 3; ++i) {
    const uint8_t component_id = data[6 + i * 3];
    const uint8_t sampling = data[7 + i * 3];
    const uint8_t quant_table = data[8 + i * 3];
    if (component_id == 1 && sampling == 0x21 && quant_table == 0) {
      component_mask |= 0x01;
    } else if (component_id == 2 && sampling == 0x11 && quant_table == 1) {
      component_mask |= 0x02;
    } else if (component_id == 3 && sampling == 0x11 && quant_table == 1) {
      component_mask |= 0x04;
    } else {
      return false;
    }
  }
  return component_mask == 0x07;
}

bool parseSos(const uint8_t *data, size_t size) {
  if (size != 10 || data[0] != 3) {
    return false;
  }
  return data[1] == 1 && data[2] == 0x00
         && data[3] == 2 && data[4] == 0x11
         && data[5] == 3 && data[6] == 0x11
         && data[7] == 0 && data[8] == 63 && data[9] == 0;
}

bool findScanEnd(const uint8_t *jpeg, size_t jpeg_size, size_t scan_start,
                 size_t *scan_end, const char **error) {
  size_t offset = scan_start;
  while (offset + 1 < jpeg_size) {
    if (jpeg[offset] != 0xff) {
      ++offset;
      continue;
    }

    uint8_t marker = jpeg[offset + 1];
    if (marker == 0x00) {
      offset += 2;
      continue;
    }
    if (marker == 0xff) {
      ++offset;
      continue;
    }
    if (marker == 0xd9) {
      *scan_end = offset;
      return true;
    }
    if (marker >= 0xd0 && marker <= 0xd7) {
      return fail("JPEG restart markers are not supported", error);
    }
    return fail("JPEG contains more than one entropy-coded scan", error);
  }
  return fail("JPEG EOI marker is missing", error);
}

}  // namespace

bool parseRfc2435Jpeg(const uint8_t *jpeg, size_t jpeg_size,
                      uint16_t expected_width, uint16_t expected_height,
                      Rfc2435JpegFrame *frame, const char **error) {
  if (error != nullptr) {
    *error = nullptr;
  }
  if (jpeg == nullptr || frame == nullptr || jpeg_size < 4) {
    return fail("JPEG input is empty", error);
  }
  *frame = Rfc2435JpegFrame{};
  if (jpeg[0] != 0xff || jpeg[1] != 0xd8) {
    return fail("JPEG SOI marker is missing", error);
  }

  bool have_sof0 = false;
  uint8_t quantization_table_mask = 0;
  uint8_t huffman_table_mask = 0;
  size_t offset = 2;

  while (offset + 1 < jpeg_size) {
    if (jpeg[offset] != 0xff) {
      return fail("invalid JPEG marker alignment", error);
    }
    while (offset < jpeg_size && jpeg[offset] == 0xff) {
      ++offset;
    }
    if (offset >= jpeg_size) {
      return fail("truncated JPEG marker", error);
    }

    uint8_t marker = jpeg[offset++];
    if (marker == 0xd8 || marker == 0xd9 || marker == 0x01
        || (marker >= 0xd0 && marker <= 0xd7)) {
      return fail("unexpected standalone JPEG marker", error);
    }
    if (offset + 2 > jpeg_size) {
      return fail("truncated JPEG segment length", error);
    }
    uint16_t segment_length = readBe16(jpeg + offset);
    if (segment_length < 2 || segment_length > jpeg_size - offset) {
      return fail("invalid JPEG segment length", error);
    }
    const uint8_t *segment = jpeg + offset + 2;
    size_t segment_size = segment_length - 2;
    size_t segment_end = offset + segment_length;

    if (marker == 0xdb) {
      if (!parseDqt(segment, segment_size, frame, &quantization_table_mask)) {
        return fail("JPEG quantization tables are unsupported", error);
      }
    } else if (marker == 0xc4) {
      if (!parseDht(segment, segment_size, &huffman_table_mask)) {
        return fail("JPEG does not use the standard Huffman tables", error);
      }
    } else if (marker == 0xc0) {
      if (have_sof0 || !parseSof0(segment, segment_size, frame)) {
        return fail("JPEG is not baseline YUV422", error);
      }
      have_sof0 = true;
    } else if (marker == 0xda) {
      if (!parseSos(segment, segment_size)) {
        return fail("JPEG scan header is unsupported", error);
      }
      if (!have_sof0 || quantization_table_mask != 0x03
          || huffman_table_mask != 0x0f) {
        return fail("JPEG headers are incomplete for RFC 2435", error);
      }
      if (frame->width != expected_width || frame->height != expected_height
          || frame->width == 0 || frame->height == 0
          || (frame->width % 8) != 0 || (frame->height % 8) != 0
          || frame->width > 2040 || frame->height > 2040) {
        return fail("JPEG dimensions do not match the RTP stream", error);
      }

      size_t scan_end = 0;
      if (!findScanEnd(jpeg, jpeg_size, segment_end, &scan_end, error)) {
        return false;
      }
      if (scan_end == segment_end) {
        return fail("JPEG entropy-coded scan is empty", error);
      }
      frame->scan_data = jpeg + segment_end;
      frame->scan_size = scan_end - segment_end;
      return true;
    } else if (marker == 0xdd) {
      return fail("JPEG restart intervals are not supported", error);
    } else if (marker >= 0xc0 && marker <= 0xcf
               && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
      return fail("JPEG is not baseline sequential DCT", error);
    }

    offset = segment_end;
  }

  return fail("JPEG SOS marker is missing", error);
}
