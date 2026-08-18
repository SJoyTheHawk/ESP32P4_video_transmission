# OV5647 1080p Descriptor Provenance

The repository-local 1920x1080 RAW10 register table in
`mipi_csi_camera/ov5647_1080p_registers.h` is imported from Espressif's
`esp-video-components` release 2.3.0, commit
`15106718de6eeb1c8d7d8b11dd5a2c7d34d5f764`.

Source file:

`esp_cam_sensor/sensors/ov5647/private_include/ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps.h`

The source table and this derivative are distributed under the Apache License
2.0. The descriptor fields match the same release's OV5647 driver:

- `MIPI_2lane_24Minput_RAW10_1920x1080_30fps`
- 24 MHz XCLK, 2 MIPI lanes, 408333500 Hz line rate
- 81666700 pixel clock, HTS 2416, VTS 1104
- GBRG Bayer order

The table is compiled as static storage because the ESP-Video driver retains a
pointer to the active sensor format after `VIDIOC_S_SENSOR_FMT` returns.
