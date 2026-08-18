#include "ov5647_1280x720_mode.h"

extern "C" {
typedef struct {
  uint16_t reg;
  uint8_t val;
} ov5647_reginfo_t;
}

#define OV5647_8BIT_MODE 0x18
#define OV5647_REG_END 0xffff
#define OV5647_IDI_CLOCK_RATE_1280x720_60FPS (160000000ULL)
#include "ov5647_1280x720_registers.h"

const esp_cam_sensor_format_t &ov5647_1280x720_sensor_format() {
  static esp_cam_sensor_isp_info_t isp_info = {
    .isp_v1_info = {
      .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
      .pclk = 160000000,
      .hts = 1896,
      .vts = 740,
      .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
    }
  };
  static const esp_cam_sensor_format_t format = {
    .name = "MIPI_2lane_24Minput_RAW8_1280x720_60fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW8,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 1280,
    .height = 720,
    .regs = ov5647_mipi_2lane_24Minput_1280x720_raw8_60fps,
    .regs_size = sizeof(ov5647_mipi_2lane_24Minput_1280x720_raw8_60fps)
                 / sizeof(ov5647_mipi_2lane_24Minput_1280x720_raw8_60fps[0]),
    .fps = 60,
    .isp_info = &isp_info,
    .mipi_info = {
      .mipi_clk = 640000000,
      .lane_num = 2,
      .line_sync_en = true,
    },
    .reserved = nullptr,
  };
  return format;
}
