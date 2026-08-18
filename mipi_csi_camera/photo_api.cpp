#include "photo_api.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t kMaxCaptureBody = 64;
constexpr size_t kJpegChunkSize = 4096;

PhotoApi *apiFromRequest(httpd_req_t *request) {
  return static_cast<PhotoApi *>(request == nullptr ? nullptr : request->user_ctx);
}

bool parseUnsignedId(const char *value, uint64_t *out) {
  if (value == nullptr || out == nullptr || *value == '\0') {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const unsigned long long parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
    return false;
  }
  *out = static_cast<uint64_t>(parsed);
  return true;
}

esp_err_t sendHttpError(httpd_req_t *request, const char *status,
                        const char *message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain");
  return httpd_resp_sendstr(request, message);
}
}  // namespace

PhotoApi::~PhotoApi() { stop(); }

bool PhotoApi::begin(PhotoStore *store, CaptureCallback callback,
                     void *callback_context, uint16_t port) {
  if (server_ != nullptr || store == nullptr || !store->ready()
      || capture_queue_ != nullptr) {
    return false;
  }
  store_ = store;
  capture_callback_ = callback;
  capture_context_ = callback_context;
  capture_queue_ = xSemaphoreCreateBinary();
  if (capture_queue_ == nullptr) {
    store_ = nullptr;
    return false;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;
  if (httpd_start(&server_, &config) != ESP_OK) {
    vSemaphoreDelete(capture_queue_);
    capture_queue_ = nullptr;
    store_ = nullptr;
    return false;
  }

  const httpd_uri_t metadata_uri = {
    .uri = "/api/photo/metadata",
    .method = HTTP_GET,
    .handler = metadataHandler,
    .user_ctx = this,
  };
  const httpd_uri_t latest_uri = {
    .uri = "/api/photo/latest.jpg",
    .method = HTTP_GET,
    .handler = latestHandler,
    .user_ctx = this,
  };
  const httpd_uri_t capture_uri = {
    .uri = "/api/photo/capture",
    .method = HTTP_POST,
    .handler = captureHandler,
    .user_ctx = this,
  };
  if (httpd_register_uri_handler(server_, &metadata_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &latest_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &capture_uri) != ESP_OK) {
    httpd_stop(server_);
    server_ = nullptr;
    vSemaphoreDelete(capture_queue_);
    capture_queue_ = nullptr;
    store_ = nullptr;
    return false;
  }

  stopping_.store(false, std::memory_order_release);
  state_.store(PhotoApiState::Ready, std::memory_order_release);
  if (xTaskCreate(captureWorker, "photo-capture", 4096, this, 2,
                  &worker_task_) != pdPASS) {
    stop();
    return false;
  }
  return true;
}

void PhotoApi::stop() {
  stopping_.store(true, std::memory_order_release);
  if (capture_queue_ != nullptr) {
    xSemaphoreGive(capture_queue_);
  }
  if (server_ != nullptr) {
    httpd_stop(server_);
    server_ = nullptr;
  }
  if (worker_task_ != nullptr) {
    vTaskDelete(worker_task_);
    worker_task_ = nullptr;
  }
  if (capture_queue_ != nullptr) {
    vSemaphoreDelete(capture_queue_);
    capture_queue_ = nullptr;
  }
  store_ = nullptr;
  capture_callback_ = nullptr;
  capture_context_ = nullptr;
  state_.store(PhotoApiState::Unavailable, std::memory_order_release);
}

bool PhotoApi::enqueueCapture() {
  if (capture_queue_ == nullptr || state() == PhotoApiState::Unavailable
      || state() == PhotoApiState::Error) {
    return false;
  }
  PhotoApiState expected = PhotoApiState::Ready;
  if (!state_.compare_exchange_strong(expected, PhotoApiState::Capturing,
                                      std::memory_order_acq_rel)) {
    return false;
  }
  if (xSemaphoreGive(capture_queue_) != pdTRUE) {
    state_.store(PhotoApiState::Ready, std::memory_order_release);
    return false;
  }
  return true;
}

void PhotoApi::setUnavailableForTest() {
  state_.store(PhotoApiState::Unavailable, std::memory_order_release);
}

void PhotoApi::restoreReadyForTest() {
  if (server_ != nullptr) {
    state_.store(PhotoApiState::Ready, std::memory_order_release);
  }
}

PhotoApiState PhotoApi::state() const {
  return state_.load(std::memory_order_acquire);
}

const char *PhotoApi::stateName() const {
  switch (state()) {
    case PhotoApiState::Ready: return "ready";
    case PhotoApiState::Capturing: return "capturing";
    case PhotoApiState::Error: return "error";
    case PhotoApiState::Unavailable: return "unavailable";
  }
  return "unknown";
}

bool PhotoApi::running() const { return server_ != nullptr; }

esp_err_t PhotoApi::metadataHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_ERR_HTTPD_INVALID_REQ : api->handleMetadata(request);
}

esp_err_t PhotoApi::latestHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_ERR_HTTPD_INVALID_REQ : api->handleLatest(request);
}

esp_err_t PhotoApi::captureHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_ERR_HTTPD_INVALID_REQ : api->handleCapture(request);
}

esp_err_t PhotoApi::handleMetadata(httpd_req_t *request) {
  if (store_ == nullptr || state() == PhotoApiState::Unavailable) {
    return sendHttpError(request, "503 Service Unavailable",
                         "photo store unavailable");
  }
  PhotoMetadata metadata;
  if (!store_->metadataSnapshot(&metadata)) {
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "photo unavailable");
  }
  char response[256];
  snprintf(response, sizeof(response),
           "{\"id\":%llu,\"width\":%lu,\"height\":%lu,"
           "\"quality\":%lu,\"size\":%lu,\"captured_ms\":%llu,"
           "\"state\":\"%s\"}",
           static_cast<unsigned long long>(metadata.photo_id),
           static_cast<unsigned long>(metadata.width),
           static_cast<unsigned long>(metadata.height),
           static_cast<unsigned long>(metadata.quality),
           static_cast<unsigned long>(metadata.size),
           static_cast<unsigned long long>(metadata.captured_ms), stateName());
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhotoApi::handleLatest(httpd_req_t *request) {
  if (store_ == nullptr || state() == PhotoApiState::Unavailable) {
    return sendHttpError(request, "503 Service Unavailable",
                         "photo store unavailable");
  }
  PhotoBlob *blob = nullptr;
  uint64_t requested_id = 0;
  char query[48] = {};
  const size_t query_len = httpd_req_get_url_query_len(request);
  if (query_len > 0) {
    if (query_len >= sizeof(query)
        || httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK
        || strncmp(query, "id=", 3) != 0
        || !parseUnsignedId(query + 3, &requested_id)) {
      return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                 "invalid photo id");
    }
    blob = store_->acquireById(requested_id);
    if (blob == nullptr) {
      return sendHttpError(request, "410 Gone", "photo expired");
    }
  } else {
    blob = store_->acquireLatest();
    if (blob == nullptr) {
      return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "photo unavailable");
    }
  }

  const uint64_t photo_id = blob->metadata().photo_id;
  char etag[40];
  snprintf(etag, sizeof(etag), "\"photo-%llu\"",
           static_cast<unsigned long long>(photo_id));
  char if_none_match[48] = {};
  if (httpd_req_get_hdr_value_str(request, "If-None-Match", if_none_match,
                                  sizeof(if_none_match)) == ESP_OK
      && strcmp(if_none_match, etag) == 0) {
    httpd_resp_set_hdr(request, "ETag", etag);
    store_->release(blob);
    httpd_resp_set_status(request, "304 Not Modified");
    return httpd_resp_send(request, nullptr, 0);
  }

  httpd_resp_set_type(request, "image/jpeg");
  httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(request, "ETag", etag);
  const uint8_t *data = blob->data();
  const size_t size = blob->size();
  esp_err_t result = ESP_OK;
  for (size_t offset = 0; offset < size && result == ESP_OK;) {
    const size_t chunk = min(kJpegChunkSize, size - offset);
    result = httpd_resp_send_chunk(request,
                                   reinterpret_cast<const char *>(data + offset),
                                   chunk);
    offset += chunk;
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nullptr, 0);
  }
  store_->release(blob);
  return result;
}

bool PhotoApi::parseCaptureBody(httpd_req_t *request) {
  if (request->content_len > kMaxCaptureBody) {
    return false;
  }
  char body[kMaxCaptureBody + 1] = {};
  size_t received = 0;
  while (received < request->content_len) {
    const int result = httpd_req_recv(request, body + received,
                                      request->content_len - received);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  size_t first = 0;
  while (first < received && isspace(static_cast<unsigned char>(body[first]))) {
    ++first;
  }
  size_t last = received;
  while (last > first && isspace(static_cast<unsigned char>(body[last - 1]))) {
    --last;
  }
  return first == last || (last - first == 2 && body[first] == '{'
                           && body[first + 1] == '}');
}

esp_err_t PhotoApi::handleCapture(httpd_req_t *request) {
  if (!parseCaptureBody(request)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "body must be empty or {}");
  }
  if (state() == PhotoApiState::Unavailable) {
    return sendHttpError(request, "503 Service Unavailable",
                         "capture unavailable");
  }
  if (state() == PhotoApiState::Capturing) {
    return sendHttpError(request, "409 Conflict", "capture busy");
  }
  if (state() == PhotoApiState::Error) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "capture error");
  }
  if (!enqueueCapture()) {
    return sendHttpError(request, "409 Conflict", "capture busy");
  }
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_status(request, "202 Accepted");
  return httpd_resp_sendstr(request, "{\"status\":\"capturing\"}");
}

void PhotoApi::captureWorker(void *argument) {
  static_cast<PhotoApi *>(argument)->runCaptureWorker();
}

void PhotoApi::runCaptureWorker() {
  while (!stopping_.load(std::memory_order_acquire)) {
    if (xSemaphoreTake(capture_queue_, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      break;
    }
    const bool captured = capture_callback_ != nullptr
                          && capture_callback_(capture_context_);
    state_.store(captured ? PhotoApiState::Ready : PhotoApiState::Error,
                 std::memory_order_release);
  }
  worker_task_ = nullptr;
  vTaskDelete(nullptr);
}

void PhotoApi::setState(PhotoApiState state) {
  state_.store(state, std::memory_order_release);
}
