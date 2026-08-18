#include "photo_api.h"
#include "web_ui.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <WiFi.h>
#include <esp_netif.h>

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
  config.max_uri_handlers = 15;
  config.lru_purge_enable = true;
  if (httpd_start(&server_, &config) != ESP_OK) {
    vSemaphoreDelete(capture_queue_);
    capture_queue_ = nullptr;
    store_ = nullptr;
    return false;
  }

  const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = rootHandler,
    .user_ctx = this,
  };
  const httpd_uri_t login_uri = {
    .uri = "/api/login",
    .method = HTTP_POST,
    .handler = loginHandler,
    .user_ctx = this,
  };
  const httpd_uri_t logout_uri = {
    .uri = "/api/logout",
    .method = HTTP_POST,
    .handler = logoutHandler,
    .user_ctx = this,
  };
  const httpd_uri_t version_uri = {
    .uri = "/api/version",
    .method = HTTP_GET,
    .handler = versionHandler,
    .user_ctx = this,
  };
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
  const httpd_uri_t resolution_get_uri = {
    .uri = "/api/stream/resolution",
    .method = HTTP_GET,
    .handler = streamResolutionGetHandler,
    .user_ctx = this,
  };
  const httpd_uri_t resolution_put_uri = {
    .uri = "/api/stream/resolution",
    .method = HTTP_PUT,
    .handler = streamResolutionPutHandler,
    .user_ctx = this,
  };
  const httpd_uri_t settings_get_uri = {
    .uri = "/api/settings",
    .method = HTTP_GET,
    .handler = settingsGetHandler,
    .user_ctx = this,
  };
  const httpd_uri_t settings_put_uri = {
    .uri = "/api/settings",
    .method = HTTP_PUT,
    .handler = settingsPutHandler,
    .user_ctx = this,
  };
  const httpd_uri_t settings_reset_uri = {
    .uri = "/api/settings/reset",
    .method = HTTP_POST,
    .handler = settingsResetHandler,
    .user_ctx = this,
  };
  const httpd_uri_t network_settings_get_uri = {
    .uri = "/api/network/settings",
    .method = HTTP_GET,
    .handler = networkSettingsGetHandler,
    .user_ctx = this,
  };
  const httpd_uri_t network_settings_put_uri = {
    .uri = "/api/network/settings",
    .method = HTTP_PUT,
    .handler = networkSettingsPutHandler,
    .user_ctx = this,
  };
  if (httpd_register_uri_handler(server_, &root_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &login_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &logout_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &version_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &metadata_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &latest_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &capture_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &resolution_get_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &resolution_put_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &settings_get_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &settings_put_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &settings_reset_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &network_settings_get_uri) != ESP_OK
      || httpd_register_uri_handler(server_, &network_settings_put_uri) != ESP_OK) {
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

void PhotoApi::setCaptureController(CaptureController *controller) {
  controller_ = controller;
}

void PhotoApi::setAuthManager(AuthManager *auth_manager) {
  auth_manager_ = auth_manager;
}

void PhotoApi::setFirmwareVersion(const char *version) {
  firmware_version_ = version;
}

esp_err_t PhotoApi::versionHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleVersion(request);
}

esp_err_t PhotoApi::handleVersion(httpd_req_t *request) {
  const char *version = firmware_version_ != nullptr ? firmware_version_ : "1.0.0";

  char response[128];
  snprintf(response, sizeof(response), "{\"status\":\"success\",\"version\":\"%s\"}", version);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

String PhotoApi::getTokenFromRequest(httpd_req_t *request) {
  // Check for session token in query parameter
  size_t buf_len = httpd_req_get_url_query_len(request) + 1;
  if (buf_len > 1) {
    char *buf = (char *)malloc(buf_len);
    if (buf != nullptr) {
      if (httpd_req_get_url_query_str(request, buf, buf_len) == ESP_OK) {
        char param[64];
        if (httpd_query_key_value(buf, "token", param, sizeof(param)) == ESP_OK) {
          String token = String(param);
          free(buf);
          return token;
        }
      }
      free(buf);
    }
  }
  return "";
}

bool PhotoApi::isAuthenticated(httpd_req_t *request) {
  if (auth_manager_ == nullptr) {
    return true; // No auth manager, allow access
  }

  String token = getTokenFromRequest(request);
  return auth_manager_->isAuthenticated(token);
}

esp_err_t PhotoApi::rootHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleRoot(request);
}

esp_err_t PhotoApi::handleRoot(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html");

  // Check authentication
  if (isAuthenticated(request)) {
    return httpd_resp_sendstr(request, WebUI::getSettingsPage());
  } else {
    return httpd_resp_sendstr(request, WebUI::getLoginPage());
  }
}

esp_err_t PhotoApi::loginHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleLogin(request);
}

esp_err_t PhotoApi::handleLogin(httpd_req_t *request) {
  if (auth_manager_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable", "Auth not configured");
  }

  char content[256];
  size_t recv_size = min(request->content_len, sizeof(content) - 1);
  int ret = httpd_req_recv(request, content, recv_size);
  if (ret <= 0) {
    return sendHttpError(request, "400 Bad Request", "Failed to read request body");
  }
  content[ret] = '\0';

  // Parse URL-encoded form data
  char username[64] = {0};
  char password[64] = {0};

  char *token = strtok(content, "&");
  while (token != nullptr) {
    if (strncmp(token, "username=", 9) == 0) {
      strncpy(username, token + 9, sizeof(username) - 1);
    } else if (strncmp(token, "password=", 9) == 0) {
      strncpy(password, token + 9, sizeof(password) - 1);
    }
    token = strtok(nullptr, "&");
  }

  if (auth_manager_->login(String(username), String(password))) {
    String session_token = auth_manager_->getCurrentToken();
    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"success\",\"message\":\"Login successful\",\"token\":\"%s\"}",
             session_token.c_str());
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
  } else {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"status\":\"error\",\"message\":\"Invalid credentials\"}");
  }
}

esp_err_t PhotoApi::logoutHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleLogout(request);
}

esp_err_t PhotoApi::handleLogout(httpd_req_t *request) {
  if (auth_manager_ != nullptr) {
    auth_manager_->logout();
  }

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"status\":\"success\",\"message\":\"Logged out\"}");
}

esp_err_t PhotoApi::networkSettingsGetHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleNetworkSettingsGet(request);
}

esp_err_t PhotoApi::handleNetworkSettingsGet(httpd_req_t *request) {
  if (!isAuthenticated(request)) {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
  }

  // Get current IP address
  char ip_str[16] = "0.0.0.0";
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif != nullptr) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
  }

  char response[512];
  snprintf(response, sizeof(response),
           "{\"status\":\"success\",\"ip_address\":\"%s\",\"dhcp\":true}",
           ip_str);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

esp_err_t PhotoApi::networkSettingsPutHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleNetworkSettingsPut(request);
}

esp_err_t PhotoApi::handleNetworkSettingsPut(httpd_req_t *request) {
  if (!isAuthenticated(request)) {
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
  }

  // For now, just acknowledge the request
  // Full implementation would require WiFi reconfiguration
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request,
    "{\"status\":\"success\",\"message\":\"Network settings updated (restart required)\"}");
}

esp_err_t PhotoApi::streamResolutionGetHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleStreamResolutionGet(request);
}

esp_err_t PhotoApi::streamResolutionPutHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? ESP_FAIL : api->handleStreamResolutionPut(request);
}

esp_err_t PhotoApi::handleStreamResolutionGet(httpd_req_t *request) {
  if (controller_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                        "Controller not available");
  }

  StreamResolution current = controller_->getCurrentResolution();
  const char *current_name = controller_->resolutionName(current);
  uint32_t width = controller_->width();
  uint32_t height = controller_->height();

  char response[256];
  snprintf(response, sizeof(response),
           "{\"current\":\"%s\",\"resolution_name\":\"%s\",\"width\":%u,\"height\":%u,"
           "\"supported\":[\"800x800\"]}",
           "800x800",
           current_name,
           width, height);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

esp_err_t PhotoApi::handleStreamResolutionPut(httpd_req_t *request) {
  if (controller_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                        "Controller not available");
  }

  if (!controller_->isBaselineRunning()) {
    return sendHttpError(request, "503 Service Unavailable",
                        "Controller not in baseline state");
  }

  char body[64] = {};
  const int content_len = request->content_len;
  if (content_len <= 0 || content_len >= static_cast<int>(sizeof(body))) {
    return sendHttpError(request, "400 Bad Request", "Invalid content length");
  }

  const int received = httpd_req_recv(request, body, content_len);
  if (received != content_len) {
    return sendHttpError(request, "400 Bad Request", "Failed to read body");
  }
  body[content_len] = '\0';

  // Simple JSON parsing for {"resolution":"vga"} or {"resolution":"720p"}
  const char *resolution_key = strstr(body, "\"resolution\"");
  if (resolution_key == nullptr) {
    return sendHttpError(request, "400 Bad Request",
                        "Missing resolution field");
  }

  const char *value_start = strchr(resolution_key, ':');
  if (value_start == nullptr) {
    return sendHttpError(request, "400 Bad Request", "Invalid JSON format");
  }
  value_start++;
  while (*value_start == ' ' || *value_start == '\"') value_start++;

  StreamResolution target;
  bool valid = false;

  if (strncmp(value_start, "vga", 3) == 0) {
    target = StreamResolution::VGA_640x480;
    valid = true;
  } else if (strncmp(value_start, "720p", 4) == 0 || strncmp(value_start, "hd", 2) == 0) {
    target = StreamResolution::HD_1280x720;
    valid = true;
  } else if (strncmp(value_start, "1080p", 5) == 0 || strncmp(value_start, "fhd", 3) == 0) {
    target = StreamResolution::FHD_1920x1080;
    valid = true;
  }

  if (!valid) {
    return sendHttpError(request, "400 Bad Request",
                        "Invalid resolution (use 'vga', '720p', 'hd', '1080p', or 'fhd')");
  }

  StreamResolution previous = controller_->getCurrentResolution();
  if (!controller_->switchResolution(target)) {
    return sendHttpError(request, "500 Internal Server Error",
                        "Resolution switch failed");
  }

  char response[256];
  snprintf(response, sizeof(response),
           "{\"status\":\"success\",\"resolution\":\"%s\","
           "\"width\":%u,\"height\":%u,\"previous\":\"%s\"}",
           "800x800",
           controller_->width(), controller_->height(),
           "800x800");

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

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
    Serial.printf("photo capture worker status=%s state=%s\n",
                  captured ? "published" : "failed", stateName());
  }
  worker_task_ = nullptr;
  vTaskDelete(nullptr);
}

void PhotoApi::setState(PhotoApiState state) {
  state_.store(state, std::memory_order_release);
}

void PhotoApi::setSettingsManager(SettingsManager *manager,
                                   CameraSettings *settings) {
  settings_manager_ = manager;
  settings_ = settings;
}

esp_err_t PhotoApi::settingsGetHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no context")
                        : api->handleSettingsGet(request);
}

esp_err_t PhotoApi::settingsPutHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no context")
                        : api->handleSettingsPut(request);
}

esp_err_t PhotoApi::settingsResetHandler(httpd_req_t *request) {
  PhotoApi *api = apiFromRequest(request);
  return api == nullptr ? httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no context")
                        : api->handleSettingsReset(request);
}

esp_err_t PhotoApi::handleSettingsGet(httpd_req_t *request) {
  if (settings_manager_ == nullptr || settings_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                         "settings not available");
  }

  if (controller_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                         "controller not available");
  }

  // Convert StreamResolution enum to index for web UI
  int resolution_index;
  // The rollback firmware exposes only the known-good 800x800 stream mode.
  resolution_index = 1;

  char response[256];
  snprintf(response, sizeof(response),
           "{\"stream_resolution\":%d,"
           "\"jpeg_quality\":%u,"
           "\"auto_start_stream\":%s,"
           "\"background_capture\":{"
           "\"enabled\":%s,"
           "\"interval_seconds\":%lu}}",
           resolution_index,
           settings_->jpeg_quality,
           settings_->auto_start_stream ? "true" : "false",
           settings_->enable_background_capture ? "true" : "false",
           static_cast<unsigned long>(settings_->capture_interval_seconds));

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

esp_err_t PhotoApi::handleSettingsPut(httpd_req_t *request) {
  if (settings_manager_ == nullptr || settings_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                         "settings not available");
  }

  if (controller_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                         "controller not available");
  }

  char body[512];
  const size_t body_len = request->content_len;
  if (body_len == 0 || body_len >= sizeof(body)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "body required");
  }

  int received = httpd_req_recv(request, body, sizeof(body) - 1);
  if (received <= 0) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "failed to read body");
  }
  body[received] = '\0';

  // Parse resolution change (accepts both index and string format)
  bool resolution_changed = false;
  StreamResolution new_resolution = settings_->stream_resolution;

  // Try integer index format first: "stream_resolution":0
  const char *res_num_key = "\"stream_resolution\":";
  const char *res_num_start = strstr(body, res_num_key);
  if (res_num_start != nullptr) {
    res_num_start += strlen(res_num_key);
    char *end = nullptr;
    long index = strtol(res_num_start, &end, 10);
    if (index >= 0 && index <= 2) {
      switch (index) {
        case 0:
          new_resolution = StreamResolution::VGA_640x480;
          resolution_changed = true;
          break;
        case 1:
          new_resolution = StreamResolution::HD_1280x720;
          resolution_changed = true;
          break;
        case 2:
          new_resolution = StreamResolution::FHD_1920x1080;
          resolution_changed = true;
          break;
      }
    }
  }

  // Try string format: "stream_resolution":"vga"
  if (!resolution_changed) {
    const char *res_key = "\"stream_resolution\":\"";
    const char *res_start = strstr(body, res_key);
    if (res_start != nullptr) {
      res_start += strlen(res_key);
      if (strncmp(res_start, "vga\"", 4) == 0) {
        new_resolution = StreamResolution::VGA_640x480;
        resolution_changed = true;
      } else if (strncmp(res_start, "720p\"", 5) == 0 || strncmp(res_start, "hd\"", 3) == 0) {
        new_resolution = StreamResolution::HD_1280x720;
        resolution_changed = true;
      } else if (strncmp(res_start, "1080p\"", 6) == 0 || strncmp(res_start, "fhd\"", 4) == 0) {
        new_resolution = StreamResolution::FHD_1920x1080;
        resolution_changed = true;
      } else {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid resolution");
      }
    }
  }

  // Parse quality change
  const char *quality_key = "\"jpeg_quality\":";
  const char *quality_start = strstr(body, quality_key);
  bool quality_changed = false;
  uint8_t new_quality = settings_->jpeg_quality;

  if (quality_start != nullptr) {
    quality_start += strlen(quality_key);
    char *end = nullptr;
    long parsed = strtol(quality_start, &end, 10);
    if (parsed >= 10 && parsed <= 100) {
      new_quality = static_cast<uint8_t>(parsed);
      quality_changed = true;
    } else {
      return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                 "jpeg_quality must be 10-100");
    }
  }

  // Apply resolution change if requested
  if (resolution_changed && new_resolution != settings_->stream_resolution) {
    if (!controller_->switchResolution(new_resolution)) {
      return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "resolution switch failed");
    }
    settings_->stream_resolution = new_resolution;
  }

  // Apply quality change if requested
  if (quality_changed) {
    settings_->jpeg_quality = new_quality;
    // Note: JPEG quality change would require controller method
  }

  // Save to NVS
  if (!settings_manager_->saveSettings(*settings_)) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "failed to save settings");
  }

  // Build response
  char response[256];
  snprintf(response, sizeof(response),
           "{\"status\":\"success\","
           "\"applied\":{"
           "\"stream_resolution\":\"%s\","
           "\"jpeg_quality\":%u},"
           "\"requires_reboot\":false}",
           controller_->resolutionName(settings_->stream_resolution),
           settings_->jpeg_quality);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, response);
}

esp_err_t PhotoApi::handleSettingsReset(httpd_req_t *request) {
  if (settings_manager_ == nullptr || settings_ == nullptr) {
    return sendHttpError(request, "503 Service Unavailable",
                         "settings not available");
  }

  if (!settings_manager_->resetToDefaults()) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "failed to reset settings");
  }

  // Reload defaults
  settings_->setDefaults();

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request,
                            "{\"status\":\"success\","
                            "\"message\":\"settings reset to defaults\"}");
}
