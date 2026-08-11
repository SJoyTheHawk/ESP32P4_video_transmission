#include "ESP32-RTSPServer.h"

void RTSPServer::wrapInHTTP(char* buffer, size_t len, char* response, size_t maxLen) {
    snprintf(response, maxLen,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/x-rtsp-tunnelled\r\n"
             "Content-Length: %d\r\n"
             "Pragma: no-cache\r\n"
             "Cache-Control: no-cache\r\n"
             "\r\n"
             "%s",
             len, buffer);
}

/**
 * @brief Handles the OPTIONS RTSP request.
 * 
 * @param request The RTSP request.
 * @param session The RTSP session.
 */

void RTSPServer::handleOptions(char* request, RTSP_Session& session) {
  char* urlStart = strstr(request, "rtsp://");
  if (urlStart) {
    char* pathStart = strchr(urlStart + 7, '/');
    if (pathStart) {
      char* pathEnd = strchr(pathStart, ' ');
      if (pathEnd) {
        *pathEnd = 0;
      }
    }
  }
  
  char response[512];
  const char* publicMethods = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n\r\n";
  
  snprintf(response, sizeof(response), 
           "RTSP/1.0 200 OK\r\n"
           "CSeq: %d\r\n"
           "%s\r\n"
           "%s",
           session.cseq, 
           dateHeader(), 
           publicMethods);
  
  if (session.isHttp) {
    char httpResponse[1024];
    wrapInHTTP(response, strlen(response), httpResponse, sizeof(httpResponse));
    write(session.httpSock, httpResponse, strlen(httpResponse));
  } else {
    write(session.sock, response, strlen(response));
  }
}

/**
 * @brief Handles the DESCRIBE RTSP request.
 * 
 * @param session The RTSP session.
 */
void RTSPServer::handleDescribe(const RTSP_Session& session) {
  const String localIp = WiFi.localIP().toString();
  char sdpDescription[512];
  int sdpLen = snprintf(sdpDescription, sizeof(sdpDescription),
                        "v=0\r\n"
                        "o=- %ld 1 IN IP4 %s\r\n"
                        "s=OV5647 RTP/JPEG\r\n"
                        "c=IN IP4 %s\r\n"
                        "t=0 0\r\n"
                        "a=control:*\r\n",
                        session.sessionID, localIp.c_str(), localIp.c_str());

  if (isVideo) {
    sdpLen += snprintf(sdpDescription + sdpLen, sizeof(sdpDescription) - sdpLen,
                       "m=video 0 RTP/AVP 26\r\n"
                       "a=control:video\r\n"
                       "a=sendonly\r\n");
  }

  const char* mediaCondition = "sendrecv"; 
  // if (haveMic && haveAmp) mediaCondition = "sendrecv"; 
  // else if (haveMic) mediaCondition = "sendonly"; 
  // else if (haveAmp) mediaCondition = "recvonly"; 
  // else mediaCondition = "inactive"; 

  if (isAudio) {
    sdpLen += snprintf(sdpDescription + sdpLen, sizeof(sdpDescription) - sdpLen,
                       "m=audio 0 RTP/AVP 97\r\n"
                       "a=rtpmap:97 L16/%lu/1\r\n"
                       "a=control:audio\r\n"
                       "a=%s\r\n", sampleRate, mediaCondition);
  }

  if (isSubtitles) {
    sdpLen += snprintf(sdpDescription + sdpLen, sizeof(sdpDescription) - sdpLen,
                       "m=text 0 RTP/AVP 98\r\n"
                       "a=rtpmap:98 t140/1000\r\n"
                       "a=control:subtitles\r\n");
  }

  char response[1024];
  int responseLen = snprintf(response, sizeof(response),
                             "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s\r\nContent-Base: rtsp://%s:%u/\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n"
                             "%s",
                             session.cseq, dateHeader(), localIp.c_str(), this->rtspPort,
                             sdpLen, sdpDescription);
  
  write(session.isHttp ? session.httpSock : session.sock, response, responseLen);
}

/**
 * @brief Handles the SETUP RTSP request.
 * 
 * @param request The RTSP request.
 * @param session The RTSP session.
 */
void RTSPServer::handleSetup(char* request, RTSP_Session& session) {
  auto sendError = [&](int status, const char* reason) {
    char response[256];
    const int responseLen = snprintf(response, sizeof(response),
        "RTSP/1.0 %d %s\r\nCSeq: %d\r\n%s\r\n\r\n",
        status, reason, session.cseq, dateHeader());
    write(session.sock, response, responseLen);
  };

  if (strstr(request, "RTP/AVP/TCP") || strstr(request, "multicast")) {
    sendError(461, "Unsupported Transport");
    return;
  }
  if (!strstr(request, "RTP/AVP") || !strstr(request, "video")) {
    sendError(461, "Unsupported Transport");
    return;
  }

  unsigned int clientRtpPort = 0;
  unsigned int clientRtcpPort = 0;
  const char* clientPortStart = strstr(request, "client_port=");
  if (!clientPortStart ||
      sscanf(clientPortStart + strlen("client_port="), "%u-%u",
             &clientRtpPort, &clientRtcpPort) != 2 ||
      clientRtpPort == 0 || clientRtpPort > UINT16_MAX ||
      clientRtcpPort == 0 || clientRtcpPort > UINT16_MAX) {
    sendError(400, "Bad Request");
    return;
  }

  if (!checkAndSetupUDP(videoUnicastSocket, false, rtpVideoPort, rtpIp) ||
      !checkAndSetupUDP(videoRtcpSocket, false, rtpVideoPort + 1, rtpIp)) {
    closeSockets();
    sendError(500, "Internal Server Error");
    return;
  }

  session.isTCP = false;
  session.isMulticast = false;
  session.cVideoPort = static_cast<uint16_t>(clientRtpPort);
  session.cVideoRtcpPort = static_cast<uint16_t>(clientRtcpPort);
  firstClientConnected = true;
  firstClientIsTCP = false;
  firstClientIsMulticast = false;
  setMaxClients(1);

  sockaddr_in peerAddress = {};
  socklen_t peerAddressLength = sizeof(peerAddress);
  char peerIp[INET_ADDRSTRLEN] = "0.0.0.0";
  if (getpeername(session.sock, reinterpret_cast<sockaddr*>(&peerAddress),
                  &peerAddressLength) == 0) {
    inet_ntop(AF_INET, &peerAddress.sin_addr, peerIp, sizeof(peerIp));
  }

  const String localIp = WiFi.localIP().toString();
  char response[512];
  const int responseLen = snprintf(response, sizeof(response),
      "RTSP/1.0 200 OK\r\n"
      "CSeq: %d\r\n"
      "%s\r\n"
      "Transport: RTP/AVP;unicast;destination=%s;source=%s;"
      "client_port=%u-%u;server_port=%u-%u\r\n"
      "Session: %lu\r\n\r\n",
      session.cseq, dateHeader(), peerIp, localIp.c_str(), clientRtpPort,
      clientRtcpPort, rtpVideoPort, rtpVideoPort + 1, session.sessionID);
  write(session.sock, response, responseLen);
}

/**
 * @brief Handles the PLAY RTSP request.
 * 
 * @param session The RTSP session.
 */
void RTSPServer::handlePlay(RTSP_Session& session) {
  session.isPlaying = true;
  this->sessions[session.sessionID] = session;
  setIsPlaying(true);

  char response[256];
  const String localIp = WiFi.localIP().toString();
  snprintf(response, sizeof(response),
           "RTSP/1.0 200 OK\r\n"
           "CSeq: %d\r\n"
           "%s\r\n"
           "Range: npt=0.000-\r\n"
           "Session: %lu\r\n"
           "RTP-Info: url=rtsp://%s:%u/video;seq=%u;rtptime=%lu\r\n\r\n",
           session.cseq,
           dateHeader(),
           session.sessionID,
           localIp.c_str(),
           this->rtspPort,
           this->videoSequenceNumber,
           this->videoTimestamp);

  write(session.isHttp ? session.httpSock : session.sock, response, strlen(response));
}

/**
 * @brief Handles the PAUSE RTSP request.
 * 
 * @param session The RTSP session.
 */
void RTSPServer::handlePause(RTSP_Session& session) {
  session.isPlaying = false;
  this->sessions[session.sessionID] = session;
  updateIsPlayingStatus();
  char response[128];
  int len = snprintf(response, sizeof(response),
                     "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %lu\r\n\r\n",
                     session.cseq, session.sessionID);
  
  write(session.isHttp ? session.httpSock : session.sock, response, len);
  RTSP_LOGD(LOG_TAG, "Session %u is now paused.", session.sessionID);
}

/**
 * @brief Handles the TEARDOWN RTSP request.
 * 
 * @param session The RTSP session.
 */
void RTSPServer::handleTeardown(RTSP_Session& session) {
  session.isPlaying = false;
  this->sessions[session.sessionID] = session;
  updateIsPlayingStatus();

  char response[128];
  int len = snprintf(response, sizeof(response),
                     "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %lu\r\n\r\n",
                     session.cseq, session.sessionID);
  
  write(session.isHttp ? session.httpSock : session.sock, response, len);

  RTSP_LOGD(LOG_TAG, "RTSP Session %u has been torn down.", session.sessionID);
}

/**
 * @brief Handles incoming RTSP requests.
 * 
 * @param sock The socket file descriptor.
 * @param clientAddr The client address.
 * @return true if the request was handled successfully, false otherwise.
 */
bool RTSPServer::handleRTSPRequest(RTSP_Session& session) {
  char *buffer = (char *)ps_malloc(RTSP_BUFFER_SIZE);
  if (!buffer) {
    RTSP_LOGE(LOG_TAG, "Failed to allocate buffer with ps_malloc");
    return false;
  }

  int totalLen = 0;
  int len = 0;

  // Read data from socket until end of RTSP header or buffer limit is reached
  while ((len = recv(session.sock, buffer + totalLen, RTSP_BUFFER_SIZE - totalLen - 1, 0)) > 0) {
    totalLen += len;
    buffer[totalLen] = '\0';
    if (strstr(buffer, "\r\n\r\n")) {
      break;
    }
    if (totalLen >= RTSP_BUFFER_SIZE) { // Adjusted for null-terminator
      RTSP_LOGE(LOG_TAG, "Request too large for buffer. Total length: %d", totalLen);
      free(buffer); // Free allocated memory
      return false;
    }
  }

  if (totalLen <= 0) {
    int err = errno;
    free(buffer);
    if (err == EWOULDBLOCK || err == EAGAIN) {
      return true;
    } else if (err == ECONNRESET || err == ENOTCONN) {
      RTSP_LOGD(LOG_TAG, "Connection reset/closed - HandleTeardown");
      // Handle teardown for current session
      this->handleTeardown(session);
      // If this is an HTTP session, find and teardown both GET and POST sessions
      if (session.isHttp && session.sessionCookie[0] != '\0') {
          // Find the paired session
          RTSP_Session* pairedSession = findSessionByCookie(session.sessionCookie);
          if (pairedSession && pairedSession != &session) {
              RTSP_LOGD(LOG_TAG, "Found paired HTTP session, handling teardown");
              this->handleTeardown(*pairedSession);
          }
      }
      
      return false;
    } else {
      RTSP_LOGE(LOG_TAG, "Error reading from socket, error: %d", err);
      return false;
    }
  }

  // Check to see if RTCP packet and ignore for now...
  buffer[totalLen] = 0; // Null-terminate the buffer
  if (buffer[0] == '$') {
    free(buffer); // Free allocated memory
    return true; 
  }

  uint8_t firstByte = buffer[0]; 
  uint8_t version = (firstByte >> 6) & 0x03;
  if (version == 2) { 
    uint8_t payloadType = buffer[1] & 0x7F;
    if (payloadType >= 200 && payloadType <= 204) {
      free(buffer); // Free allocated memory
      return true;
    }
    free(buffer); // Free allocated memory
    return true;
  }

  // Check if the request is base64 encoded FIRST
  RTSP_LOGD(LOG_TAG, "Checking if base64 encoded");
  
  if (isBase64Encoded(buffer, totalLen)) {
    RTSP_LOGD(LOG_TAG, "Buffer is base64 encoded, decoding...");
    char* decodedBuffer = (char*)malloc(RTSP_BUFFER_SIZE);
    if (!decodedBuffer) {
      RTSP_LOGE(LOG_TAG, "Failed to allocate memory for decoded buffer");
      free(buffer);
      return false;
    }

    size_t decodedLen;
    if (decodeBase64(buffer, totalLen, decodedBuffer, &decodedLen)) {
      RTSP_LOGD(LOG_TAG, "Decoded buffer: %s", decodedBuffer);
      free(buffer);
      buffer = decodedBuffer;
      totalLen = decodedLen;
    } else {
      RTSP_LOGE(LOG_TAG, "Failed to decode base64 buffer");
      free(decodedBuffer);
      free(buffer);
      return false;
    }
  }

  int cseq = captureCSeq(buffer);
  if (cseq == -1) {
    RTSP_LOGE(LOG_TAG, "CSeq not found in request: %s", buffer);
    write(session.sock, "RTSP/1.0 400 Bad Request\r\n\r\n", 29);
    free(buffer); // Free allocated memory
    return true;
  }

  session.cseq = cseq;

  // Extract session ID using the provided function
  uint32_t sessionID = extractSessionID(buffer);
  if (sessionID != 0 && sessions.find(sessionID) != sessions.end()) {
    session.sessionID = sessionID;
  }

  // Authentication check
  if (authEnabled) {
    char* authHeader = strstr(buffer, "Authorization: Basic ");
    if (!authHeader) {
      sendUnauthorizedResponse(session);
      free(buffer); // Free allocated memory
      return true;
    } else {
      authHeader += 21; // Move pointer to the base64 encoded credentials
      char* authEnd = strstr(authHeader, "\r\n");
      if (authEnd) {
        *authEnd = 0; // Null-terminate the base64 string
        if (strcmp(authHeader, base64Credentials) != 0) {
          sendUnauthorizedResponse(session);
          free(buffer); // Free allocated memory
          return true;
        } else {
          // Remove the Authorization header from the buffer before continuing
          memmove(authHeader - 21, authEnd + 2, strlen(authEnd + 2) + 1);
        }
      } else {
        sendUnauthorizedResponse(session);
        free(buffer); // Free allocated memory
        return true;
      }
    }
  }

  const bool teardownRequested = strncmp(buffer, "TEARDOWN", 8) == 0;
  handleRTSPCommand(buffer, session);

  free(buffer);
  return !teardownRequested;
}

void RTSPServer::sendUnauthorizedResponse(RTSP_Session& session) {
  char response[256];
  snprintf(response, sizeof(response),
           "RTSP/1.0 401 Unauthorized\r\n"
           "CSeq: %d\r\n"
           "WWW-Authenticate: Basic realm=\"ESP32\"\r\n\r\n",
           session.cseq);
  
  write(session.isHttp ? session.httpSock : session.sock, response, strlen(response));
  RTSP_LOGW(LOG_TAG, "Sent 401 Unauthorized response to client.");
}

void RTSPServer::handleRTSPCommand(char* command, RTSP_Session& session) {
  if (strncmp(command, "OPTIONS", 7) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Options");
    handleOptions(command, session);
  } else if (strncmp(command, "DESCRIBE", 8) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Describe");
    handleDescribe(session);
  } else if (strncmp(command, "SETUP", 5) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Setup");
    handleSetup(command, session);
  } else if (strncmp(command, "PLAY", 4) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Play");
    handlePlay(session);
  } else if (strncmp(command, "TEARDOWN", 8) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Teardown");
    handleTeardown(session);
  } else if (strncmp(command, "PAUSE", 5) == 0) {
    RTSP_LOGD(LOG_TAG, "Handle RTSP Pause");
    handlePause(session);
  } else if (strncmp(command, "GET_PARAMETER", 13) == 0) {
    char response[192];
    const int responseLen = snprintf(response, sizeof(response),
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %lu\r\n\r\n",
        session.cseq, session.sessionID);
    write(session.sock, response, responseLen);
  } else {
    RTSP_LOGW(LOG_TAG, "Unknown RTSP method: %s", command);
    char response[160];
    const int responseLen = snprintf(response, sizeof(response),
        "RTSP/1.0 405 Method Not Allowed\r\nCSeq: %d\r\n\r\n",
        session.cseq);
    write(session.sock, response, responseLen);
  }
}

bool RTSPServer::isBase64Encoded(const char* buffer, size_t length) {
    // First check for spaces - if found, not base64
    for (size_t i = 0; i < length; i++) {
        if (isspace(buffer[i])) {
            return false;
        }
    }

    // Now check if it's valid base64
    if (length % 4 != 0) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        if (!isalnum(buffer[i]) && 
            buffer[i] != '+' && 
            buffer[i] != '/' && 
            buffer[i] != '=') {
            return false;
        }
    }

    return true;
}

void RTSPServer::extractSessionCookie(const char* buffer, char* sessionCookie, size_t maxLen) {
    const char* cookieHeader = strstr(buffer, "x-sessioncookie:");
    if (cookieHeader) {
        cookieHeader += strlen("x-sessioncookie:");
        while (*cookieHeader == ' ') cookieHeader++;
        const char* end = strstr(cookieHeader, "\r\n");
        size_t len = end ? (size_t)(end - cookieHeader) : strlen(cookieHeader);
        len = len < maxLen ? len : maxLen - 1;
        strncpy(sessionCookie, cookieHeader, len);
        sessionCookie[len] = '\0';
    } else {
        sessionCookie[0] = '\0';
    }
}

RTSP_Session* RTSPServer::findSessionByCookie(const char* cookie) {
    for (auto& pair : sessions) {
        if (strcmp(pair.second.sessionCookie, cookie) == 0) {
            return &pair.second;
        }
    }
    return nullptr;
}
