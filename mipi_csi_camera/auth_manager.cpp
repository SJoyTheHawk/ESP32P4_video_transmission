#include "auth_manager.h"

AuthManager::AuthManager() {
  // Set default credentials
  username_ = "admin";
  password_ = "admin";
  current_token_ = "";
}

String AuthManager::generateToken() {
  const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const int tokenLength = 32;
  String token = "";

  for (int i = 0; i < tokenLength; i++) {
    int index = random(0, sizeof(charset) - 1);
    token += charset[index];
  }

  return token;
}

bool AuthManager::login(const String& username, const String& password) {
  if (username == username_ && password == password_) {
    current_token_ = generateToken();
    return true;
  }
  return false;
}

bool AuthManager::isAuthenticated(const String& token) {
  if (current_token_.length() == 0) {
    return false;
  }
  return token == current_token_;
}

void AuthManager::logout() {
  current_token_ = "";
}

void AuthManager::setCredentials(const String& username, const String& password) {
  username_ = username;
  password_ = password;
}
