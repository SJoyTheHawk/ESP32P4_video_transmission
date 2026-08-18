#include "ov5647_800x640_mode.h"

extern "C" {
typedef struct {
  uint16_t reg;
  uint8_t val;
} ov5647_reginfo_t;
}

#define OV5647_8BIT_MODE 0x18
#define OV5647_REG_END 0xffff
#define OV5647_IDI_CLOCK_RATE_800x640_50FPS (100000000ULL)
#include "ov5647_800x640_registers.h"

const esp_cam_sensor_format_t &ov5647_800x640_sensor_format() {
  static const esp_cam_sensor_isp_info_t isp_info = {
    .isp_v1_info = {
      .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
      .pclk = 100000000,
      .hts = 1896,
      .vts = 984,
      .bayer_type = ESP_CAM_SENSOR_BAYER_GBRG,
    },
  };
  static const esp_cam_sensor_format_t format = {
    .name = "MIPI_2lane_24Minput_RAW8_800x640_50fps",
    .format = ESP_CAM_SENSOR_PIXFORMAT_RAW8,
    .port = ESP_CAM_SENSOR_MIPI_CSI,
    .xclk = 24000000,
    .width = 800,
    .height = 640,
    .regs = ov5647_mipi_2lane_24Minput_800x640_raw8_50fps,
    .regs_size = sizeof(ov5647_mipi_2lane_24Minput_800x640_raw8_50fps)
                 / sizeof(ov5647_mipi_2lane_24Minput_800x640_raw8_50fps[0]),
    .fps = 50,
    .isp_info = &isp_info,
    .mipi_info = {
      .mipi_clk = 400000000,
      .lane_num = 2,
      .line_sync_en = true,
    },
    .reserved = nullptr,
  };
  return format;
}
