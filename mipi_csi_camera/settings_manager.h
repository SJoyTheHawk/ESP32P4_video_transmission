#ifndef SETTINGS_MANAGER_H_
#define SETTINGS_MANAGER_H_

#include <Preferences.h>
#include "settings.h"

class SettingsManager {
 public:
  SettingsManager() = default;
  ~SettingsManager() = default;

  bool begin(const char* nvs_namespace = "camera");
  void end();

  bool loadSettings(CameraSettings& settings);
  bool saveSettings(const CameraSettings& settings);
  bool resetToDefaults();

 private:
  Preferences prefs_;
  bool initialized_ = false;

  static constexpr const char* kSettingsKey = "config";
};

#endif  // SETTINGS_MANAGER_H_
