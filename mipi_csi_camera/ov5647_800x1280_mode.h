#pragma once

#include <esp_cam_sensor_types.h>

// Descriptor and register table imported from Espressif esp_cam_sensor
// component (Apache-2.0), matching Arduino-ESP32 3.3.11's ESP-Video ABI.
// Source: reference/17_mipicamera/managed_components/espressif__esp_cam_sensor
// Commit: 7c343dc478f73e3234ed898eb358accd8de92ff7
const esp_cam_sensor_format_t &ov5647_800x1280_sensor_format();
