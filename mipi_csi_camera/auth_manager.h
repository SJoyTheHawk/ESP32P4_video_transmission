#pragma once

#include <Arduino.h>

class AuthManager {
public:
  AuthManager();

  // Generate a random session token
  String generateToken();

  // Validate credentials and create session
  bool login(const String& username, const String& password);

  // Check if a token is valid
  bool isAuthenticated(const String& token);

  // Logout - invalidate token
  void logout();

  // Get default credentials
  const char* getDefaultUsername() const { return "admin"; }
  const char* getDefaultPassword() const { return "admin"; }

  // Set custom credentials (for future use with EEPROM)
  void setCredentials(const String& username, const String& password);

  // Get current session token
  String getCurrentToken() const { return current_token_; }

private:
  String current_token_;
  String username_;
  String password_;
};
