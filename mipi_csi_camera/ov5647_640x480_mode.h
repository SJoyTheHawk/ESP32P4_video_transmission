/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5647 VGA (640x480) mode - proper native sensor mode with binning
 * Based on OV5647 datasheet standard configurations
 */

#ifndef OV5647_640X480_MODE_H
#define OV5647_640X480_MODE_H

#include <esp_cam_sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

const esp_cam_sensor_format_t &ov5647_640x480_sensor_format();

#ifdef __cplusplus
}
#endif

#endif  // OV5647_640X480_MODE_H
