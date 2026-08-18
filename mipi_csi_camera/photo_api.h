#pragma once

#include <Arduino.h>
#include <atomic>
#include <esp_http_server.h>
#include "photo_store.h"

enum class PhotoApiState : uint8_t {
  Ready,
  Capturing,
  Error,
  Unavailable,
};

class PhotoApi {
public:
  using CaptureCallback = bool (*)(void *context);

  PhotoApi() = default;
  ~PhotoApi();
  PhotoApi(const PhotoApi &) = delete;
  PhotoApi &operator=(const PhotoApi &) = delete;

  bool begin(PhotoStore *store, CaptureCallback callback = nullptr,
             void *callback_context = nullptr, uint16_t port = 80);
  void stop();
  bool enqueueCapture();
  void setUnavailableForTest();
  void restoreReadyForTest();
  PhotoApiState state() const;
  const char *stateName() const;
  bool running() const;

private:
  static esp_err_t metadataHandler(httpd_req_t *request);
  static esp_err_t latestHandler(httpd_req_t *request);
  static esp_err_t captureHandler(httpd_req_t *request);
  static void captureWorker(void *argument);

  esp_err_t handleMetadata(httpd_req_t *request);
  esp_err_t handleLatest(httpd_req_t *request);
  esp_err_t handleCapture(httpd_req_t *request);
  bool parseCaptureBody(httpd_req_t *request);
  void runCaptureWorker();
  void setState(PhotoApiState state);

  httpd_handle_t server_ = nullptr;
  PhotoStore *store_ = nullptr;
  CaptureCallback capture_callback_ = nullptr;
  void *capture_context_ = nullptr;
  SemaphoreHandle_t capture_queue_ = nullptr;
  TaskHandle_t worker_task_ = nullptr;
  std::atomic<PhotoApiState> state_{PhotoApiState::Unavailable};
  std::atomic<bool> stopping_{false};
};
