#include "ov5647_5mp_mode.h"

extern "C" {
typedef struct {
  uint16_t reg;
  uint8_t val;
} ov5647_reginfo_t;
}

#define OV5647_REG_END 0xffff
#include "ov5647_5mp_registers.h"

const esp_cam_sensor_format_t &ov5647_5mp_sensor_format() {
  static esp_cam_sensor_isp_info_t isp_info = {
    .isp_v1_info = {
      .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
      .pclk = 87500000,
      .hts = 2844,
      .vts = 1968,
      .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
    }
  };

  static const esp_cam_sensor_format_t format = {
    .name = "MIPI_2lane_24Minput_RAW10_2592x1944_15fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 2592,
    .height = 1944,
    .regs = ov5647_mipi_2lane_24Minput_2592x1944_raw10,
    .regs_size = sizeof(ov5647_mipi_2lane_24Minput_2592x1944_raw10)
                 / sizeof(ov5647_mipi_2lane_24Minput_2592x1944_raw10[0]),
    .fps = 15,
    .isp_info = &isp_info,
    .mipi_info = {
      .mipi_clk = 700000000,
      .lane_num = 2,
      .line_sync_en = true,
    },
    .reserved = nullptr,
  };
  return format;
}
