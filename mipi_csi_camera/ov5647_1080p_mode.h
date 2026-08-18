#pragma once

#include <esp_cam_sensor_types.h>

// Descriptor and register table imported from Espressif esp-video-components
// 2.3.0 (Apache-2.0), matching Arduino-ESP32 3.3.11's ESP-Video ABI.
const esp_cam_sensor_format_t &ov5647_1080p_sensor_format();
