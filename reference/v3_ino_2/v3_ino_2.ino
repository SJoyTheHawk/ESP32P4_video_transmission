/*
 * ESP32-S3 + OV2640 视频流推送到 Python 服务器
 * 
 * 功能：
 *   - 连接 WiFi
 *   - 初始化 OV2640 摄像头 (800x600 JPEG)
 *   - 每帧通过 HTTP POST 推送到 Python 服务器
 *   - 服务器可下发命令：拍照 / 开始录像 / 停止录像 / 切换分辨率
 *
 * 使用方法：
 *   1. 修改下方 WIFI_SSID / WIFI_PASS / SERVER_IP 为你的实际参数
 *   2. Arduino IDE 中选择 ESP32S3 开发板，启用 PSRAM (Octal)
 *   3. 上传本 sketch
 *
 * 引脚定义根据用户提供的丝印图修改
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

// ==================== 用户配置 ====================
#define WIFI_SSID     "XIMS2"
#define WIFI_PASS     "Ns203Ns203."
#define SERVER_IP     "192.168.2.51"   // Python 服务器 IP
#define SERVER_PORT   "8000"
#define STREAM_URL    "http://" SERVER_IP ":" SERVER_PORT "/stream"

// ==================== 摄像头引脚（按用户丝印图） ====================
#define PWDN_GPIO_NUM     15   // PWON
#define RESET_GPIO_NUM    16   // RST
#define XCLK_GPIO_NUM     -1
#define SIOD_GPIO_NUM     2    // SDA
#define SIOC_GPIO_NUM     1    // SCL
#define Y9_GPIO_NUM       14   // D7(DZ)
#define Y8_GPIO_NUM       13   // D6
#define Y7_GPIO_NUM       12   // D5
#define Y6_GPIO_NUM       11   // D4
#define Y5_GPIO_NUM       10   // D3
#define Y4_GPIO_NUM        9   // D2
#define Y3_GPIO_NUM        8   // D1
#define Y2_GPIO_NUM        7   // D0
#define VSYNC_GPIO_NUM     3   // VSYNC
#define HREF_GPIO_NUM      4   // HREF
#define PCLK_GPIO_NUM      5   // DCLK

// ==================== LED 反馈引脚 ====================
#define LED_GPIO_NUM       48  // 板载 LED（根据模块调整）

// ==================== 全局变量 ====================
#define FRAME_INTERVAL_MS  50   // 20fps = 50ms/帧
#define POST_TIMEOUT_MS    2000 // HTTP POST 超时

static unsigned long lastFrameTime = 0;
static framesize_t currentResolution = FRAMESIZE_SVGA;
static int currentQuality = 12;

// ==================== 摄像头初始化 ====================
bool initCamera(framesize_t resolution = FRAMESIZE_SVGA, int quality = 12) {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_count     = 2;       // 双缓冲提高帧率
    config.jpeg_quality = quality;
    config.frame_size   = resolution;

    // 检查 PSRAM
    if (psramFound()) {
        config.fb_count = 2;
        Serial0.println("[CAM] PSRAM found, using 2 frame buffers");
    } else {
        Serial0.println("[CAM] WARNING: PSRAM not found!");
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial0.printf("[CAM] Init FAILED: 0x%x\n", err);
        return false;
    }
    
    // 设置传感器参数
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 0);        // 不翻转
        s->set_hmirror(s, 0);      // 不镜像
        s->set_brightness(s, 0);   // 亮度 0
        s->set_saturation(s, 0);   // 饱和度 0
    }
    
    Serial0.printf("[CAM] Init OK | Resolution: %d | Quality: %d\n", resolution, quality);
    return true;
}

// ==================== 重新初始化摄像头 ====================
bool reinitCamera(framesize_t resolution, int quality) {
    Serial0.printf("[CAM] Reinit: resolution=%d, quality=%d\n", resolution, quality);
    
    // 释放当前摄像头
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
        Serial0.printf("[CAM] Deinit FAILED: 0x%x\n", err);
        return false;
    }
    
    delay(100);  // 等待硬件稳定
    
    // 重新初始化
    if (!initCamera(resolution, quality)) {
        Serial0.println("[CAM] Reinit FAILED");
        return false;
    }
    
    currentResolution = resolution;
    currentQuality = quality;
    Serial0.println("[CAM] Reinit OK");
    return true;
}

// ==================== LED 闪烁反馈 ====================
void flashLED(int times = 2, int delayMs = 100) {
    pinMode(LED_GPIO_NUM, OUTPUT);
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_GPIO_NUM, HIGH);
        delay(delayMs);
        digitalWrite(LED_GPIO_NUM, LOW);
        delay(delayMs);
    }
}

// ==================== WiFi 连接 ====================
void connectWiFi() {
    Serial0.printf("[WIFI] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(false);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial0.print(".");
        if (++retries > 40) {
            Serial0.println("\n[WIFI] Failed! Restarting...");
            ESP.restart();
        }
    }
    Serial0.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ==================== 解析服务器命令 ====================
void handleCommand(const String& resp) {
    // 简单的 JSON 解析，查找 "cmd" 字段
    int cmdIdx = resp.indexOf("\"cmd\"");
    if (cmdIdx < 0) return;
    
    // 解析命令类型
    if (resp.indexOf("\"photo\"") >= 0) {
        Serial0.println("[CMD] photo -> LED flash");
        flashLED(3, 80);
    }
    else if (resp.indexOf("\"set_resolution\"") >= 0) {
        // 解析分辨率值
        int valIdx = resp.indexOf("\"value\"");
        if (valIdx >= 0) {
            framesize_t newRes = currentResolution;
            int newQuality = currentQuality;
            
            if (resp.indexOf("\"QVGA\"") >= 0) newRes = FRAMESIZE_QVGA;
            else if (resp.indexOf("\"VGA\"") >= 0) newRes = FRAMESIZE_VGA;
            else if (resp.indexOf("\"SVGA\"") >= 0) newRes = FRAMESIZE_SVGA;
            else if (resp.indexOf("\"XGA\"") >= 0) newRes = FRAMESIZE_XGA;
            else if (resp.indexOf("\"UXGA\"") >= 0) newRes = FRAMESIZE_UXGA;
            
            // 解析质量值
            int qIdx = resp.indexOf("\"quality\"");
            if (qIdx >= 0) {
                int start = resp.indexOf(":", qIdx) + 1;
                int end = resp.indexOf(",", start);
                if (end < 0) end = resp.indexOf("}", start);
                if (start > 0 && end > start) {
                    newQuality = resp.substring(start, end).toInt();
                    if (newQuality < 10) newQuality = 10;
                    if (newQuality > 63) newQuality = 63;
                }
            }
            
            Serial0.printf("[CMD] set_resolution: %d, quality: %d\n", newRes, newQuality);
            reinitCamera(newRes, newQuality);
        }
    }
    else if (resp.indexOf("\"get_status\"") >= 0) {
        Serial0.printf("[CMD] get_status: res=%d, quality=%d\n", currentResolution, currentQuality);
    }
}

// ==================== 发送帧到服务器 ====================
void sendFrame(camera_fb_t *fb) {
    HTTPClient http;
    http.begin(STREAM_URL);
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(POST_TIMEOUT_MS);

    int code = http.POST(fb->buf, fb->len);

    if (code == 200) {
        String resp = http.getString();
        // 解析服务器返回的 JSON 命令
        if (resp.indexOf("\"cmd\"") >= 0) {
            handleCommand(resp);
        }
    } else if (code > 0) {
        Serial0.printf("[HTTP] Code: %d\n", code);
    } else {
        Serial0.printf("[HTTP] Error: %s\n", http.errorToString(code).c_str());
    }
    http.end();
}

// ==================== setup ====================
void setup() {
    Serial0.begin(115200);
    Serial0.println("\n===========================");
    Serial0.println(" ESP32-S3 Camera Streamer");
    Serial0.println("===========================");

    // 初始化 LED
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, LOW);

    if (!initCamera(currentResolution, currentQuality)) {
        Serial0.println("Camera init failed, halting.");
        while (true) delay(1000);
    }

    connectWiFi();

    // 启动时闪烁 LED 表示就绪
    flashLED(1, 200);
    
    Serial0.println("[SYS] Starting stream loop...");
}

// ==================== loop ====================
void loop() {
    unsigned long now = millis();

    // 20fps 节流
    if (now - lastFrameTime < FRAME_INTERVAL_MS) {
        delay(2);
        return;
    }
    lastFrameTime = now;

    // WiFi 断线自动重连
    if (WiFi.status() != WL_CONNECTED) {
        Serial0.println("[WIFI] Disconnected, reconnecting...");
        connectWiFi();
        return;
    }

    // 采集一帧
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial0.println("[CAM] Capture failed!");
        delay(100);
        return;
    }

    // 推送到服务器
    sendFrame(fb);

    // 释放帧缓冲
    esp_camera_fb_return(fb);
}
