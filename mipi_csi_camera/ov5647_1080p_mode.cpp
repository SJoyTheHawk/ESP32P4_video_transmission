#include "ov5647_1080p_mode.h"

extern "C" {
typedef struct {
  uint16_t reg;
  uint8_t val;
} ov5647_reginfo_t;
}

#define OV5647_10BIT_MODE 0x1A
#define OV5647_REG_END 0xffff
#include "ov5647_1080p_registers.h"

const esp_cam_sensor_format_t &ov5647_1080p_sensor_format() {
  static const esp_cam_sensor_isp_info_t isp_info = {
    .isp_v1_info = {
      .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
      .pclk = 81666700,
      .hts = 2416,
      .vts = 1104,
      .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
    },
  };
  static const esp_cam_sensor_format_t format = {
    .name = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1920,
    .height = 1080,
    .regs = ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps,
    .regs_size = sizeof(ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps)
                 / sizeof(ov5647_mipi_2lane_24Minput_1920x1080_raw10_30fps[0]),
    .fps = 30,
    .isp_info = &isp_info,
    .mipi_info = {
      .mipi_clk = 408333500,
      .lane_num = 2,
      .line_sync_en = true,
    },
    .reserved = nullptr,
  };
  return format;
}
