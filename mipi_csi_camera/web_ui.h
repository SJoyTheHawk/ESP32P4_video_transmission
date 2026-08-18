/**
 * Web UI - HTML interface for camera settings and stream control
 * Phase 9: Settings page with resolution selector and quality controls
 */

#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

class WebUI {
public:
  // Get the HTML content for the settings page
  static const char* getSettingsPage();

  // Get the HTML content for the login page
  static const char* getLoginPage();
};

#endif // WEB_UI_H
