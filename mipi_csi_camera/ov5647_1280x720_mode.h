/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5647 720p (1280x720) mode - proper native sensor mode
 */

#ifndef OV5647_1280X720_MODE_H
#define OV5647_1280X720_MODE_H

#include <esp_cam_sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

const esp_cam_sensor_format_t &ov5647_1280x720_sensor_format();

#ifdef __cplusplus
}
#endif

#endif  // OV5647_1280X720_MODE_H
