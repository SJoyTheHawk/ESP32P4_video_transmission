#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <cstdint>
#include "capture_controller.h"

struct CameraSettings {
  StreamResolution stream_resolution;
  uint8_t jpeg_quality;
  bool auto_start_stream;

  // Background capture settings (optional for future use)
  bool enable_background_capture;
  uint32_t capture_interval_seconds;

  // Magic number for validation
  uint32_t magic;

  static constexpr uint32_t kMagicNumber = 0xCAFEBEEF;

  bool isValid() const {
    return magic == kMagicNumber &&
           jpeg_quality >= 10 && jpeg_quality <= 100 &&
           capture_interval_seconds >= 10 && capture_interval_seconds <= 86400;
  }

  void setDefaults() {
    stream_resolution = StreamResolution::XVGA_800x800;
    jpeg_quality = 80;
    auto_start_stream = true;
    enable_background_capture = false;
    capture_interval_seconds = 300;  // 5 minutes default
    magic = kMagicNumber;
  }
};

#endif  // SETTINGS_H_
