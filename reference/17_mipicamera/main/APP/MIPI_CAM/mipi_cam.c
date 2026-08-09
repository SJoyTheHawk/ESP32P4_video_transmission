/**
 ******************************************************************************
 * @file        mipi_cam.c
 * @brief       Headless OV5647 MIPI-CSI capture and frame validation.
 ******************************************************************************
 */

#include "mipi_cam.h"

#include <inttypes.h>
#include <string.h>
#include <sys/errno.h>

#include "esp_timer.h"

static const char *mipi_cam_tag = "mipi_cam";

#define CAMERA_BUFFER_COUNT       2
#define CAMERA_LOG_INTERVAL       50
#define FRAME_SAMPLE_COUNT        64

static int s_video_fd = -1;
static void *s_camera_buffers[CAMERA_BUFFER_COUNT];

static uint32_t frame_sample_hash(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u;
    size_t stride = length / FRAME_SAMPLE_COUNT;

    if (stride == 0) {
        stride = 1;
    }

    for (size_t i = 0; i < length && i / stride < FRAME_SAMPLE_COUNT; i += stride) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static void camera_capture_task(void *arg)
{
    (void)arg;

    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    struct v4l2_buffer buf;
    int frame_count = 0;
    int64_t interval_start = esp_timer_get_time();

    if (ioctl(s_video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(mipi_cam_tag, "failed to read capture format: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(mipi_cam_tag, "capture format: %" PRIu32 "x%" PRIu32 " fourcc=0x%" PRIx32
             " sizeimage=%" PRIu32,
             format.fmt.pix.width,
             format.fmt.pix.height,
             format.fmt.pix.pixelformat,
             format.fmt.pix.sizeimage);

    ESP_ERROR_CHECK(camera_set_bufs(s_video_fd, CAMERA_BUFFER_COUNT, NULL));
    ESP_ERROR_CHECK(camera_get_bufs(CAMERA_BUFFER_COUNT, s_camera_buffers));
    ESP_ERROR_CHECK(camera_stream_start(s_video_fd));

    while (true) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(mipi_cam_tag, "failed to receive frame: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (buf.index >= CAMERA_BUFFER_COUNT || s_camera_buffers[buf.index] == NULL) {
            ESP_LOGE(mipi_cam_tag, "invalid buffer index=%" PRIu32, buf.index);
            break;
        }

        uint32_t mapped_size = camera_get_buf_size(buf.index);
        bool valid_length = buf.bytesused > 0 && buf.bytesused <= mapped_size;
        uint32_t sample_hash = valid_length
                             ? frame_sample_hash(s_camera_buffers[buf.index], buf.bytesused)
                             : 0;

        frame_count++;
        if (frame_count == 1 || frame_count % CAMERA_LOG_INTERVAL == 0) {
            int64_t now = esp_timer_get_time();
            double fps = frame_count > 1
                       ? (double)(CAMERA_LOG_INTERVAL * 1000000LL) / (double)(now - interval_start)
                       : 0.0;

            ESP_LOGI(mipi_cam_tag,
                     "frame=%d index=%" PRIu32 " bytesused=%" PRIu32 " mapped=%" PRIu32
                     " valid=%s sample_hash=0x%08" PRIx32 " fps=%.2f",
                     frame_count,
                     buf.index,
                     buf.bytesused,
                     mapped_size,
                     valid_length ? "yes" : "no",
                     sample_hash,
                     fps);
            interval_start = now;
        }

        if (!valid_length) {
            ESP_LOGE(mipi_cam_tag,
                     "invalid frame length: bytesused=%" PRIu32 " mapped=%" PRIu32,
                     buf.bytesused,
                     mapped_size);
        }

        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(mipi_cam_tag, "failed to requeue frame: errno=%d", errno);
            break;
        }
    }

    camera_stream_stop(s_video_fd);
    vTaskDelete(NULL);
}

esp_err_t mipi_cam_init(void)
{
    esp_err_t ret = app_video_main(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(mipi_cam_tag, "video main init failed: 0x%x", ret);
        return ret;
    }

    s_video_fd = app_video_open(0);
    if (s_video_fd < 0) {
        ESP_LOGE(mipi_cam_tag, "video cam open failed");
        return ESP_FAIL;
    }

    ret = app_video_init(s_video_fd, APP_VIDEO_FMT_RGB565);
    if (ret != ESP_OK) {
        ESP_LOGE(mipi_cam_tag, "video cam init failed: 0x%x", ret);
        close(s_video_fd);
        s_video_fd = -1;
        return ret;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        camera_capture_task,
        "camera capture",
        4096,
        NULL,
        4,
        NULL,
        0);
    if (task_ret != pdPASS) {
        ESP_LOGE(mipi_cam_tag, "failed to create camera capture task");
        close(s_video_fd);
        s_video_fd = -1;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
