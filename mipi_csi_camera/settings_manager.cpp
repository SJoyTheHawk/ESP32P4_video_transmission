#include "settings_manager.h"
#include <Arduino.h>

bool SettingsManager::begin(const char* nvs_namespace) {
  if (initialized_) {
    return true;
  }

  if (!prefs_.begin(nvs_namespace, false)) {
    Serial.println("settings_manager: failed to initialize NVS");
    return false;
  }

  initialized_ = true;
  Serial.printf("settings_manager: initialized namespace=%s\n", nvs_namespace);
  return true;
}

void SettingsManager::end() {
  if (initialized_) {
    prefs_.end();
    initialized_ = false;
  }
}

bool SettingsManager::loadSettings(CameraSettings& settings) {
  if (!initialized_) {
    Serial.println("settings_manager: not initialized");
    return false;
  }

  size_t required_size = sizeof(CameraSettings);
  size_t actual_size = prefs_.getBytesLength(kSettingsKey);

  if (actual_size == 0) {
    Serial.println("settings_manager: no saved settings found");
    return false;
  }

  if (actual_size != required_size) {
    Serial.printf("settings_manager: size mismatch expected=%u actual=%u\n",
                  required_size, actual_size);
    return false;
  }

  size_t read_bytes = prefs_.getBytes(kSettingsKey, &settings, required_size);
  if (read_bytes != required_size) {
    Serial.printf("settings_manager: read failed expected=%u actual=%u\n",
                  required_size, read_bytes);
    return false;
  }

  if (!settings.isValid()) {
    Serial.println("settings_manager: loaded settings failed validation");
    return false;
  }

  Serial.println("settings_manager: loaded valid settings from NVS");
  return true;
}

bool SettingsManager::saveSettings(const CameraSettings& settings) {
  if (!initialized_) {
    Serial.println("settings_manager: not initialized");
    return false;
  }

  if (!settings.isValid()) {
    Serial.println("settings_manager: refusing to save invalid settings");
    return false;
  }

  size_t written = prefs_.putBytes(kSettingsKey, &settings, sizeof(CameraSettings));
  if (written != sizeof(CameraSettings)) {
    Serial.printf("settings_manager: write failed expected=%u actual=%u\n",
                  sizeof(CameraSettings), written);
    return false;
  }

  Serial.println("settings_manager: saved settings to NVS");
  return true;
}

bool SettingsManager::resetToDefaults() {
  if (!initialized_) {
    Serial.println("settings_manager: not initialized");
    return false;
  }

  CameraSettings defaults;
  defaults.setDefaults();

  if (!saveSettings(defaults)) {
    Serial.println("settings_manager: failed to save defaults");
    return false;
  }

  Serial.println("settings_manager: reset to defaults");
  return true;
}
