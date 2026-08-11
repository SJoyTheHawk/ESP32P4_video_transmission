#include "ESP32-RTSPServer.h"
#include "rfc2435_jpeg.h"

#include <esp_timer.h>

#ifdef RTSP_VIDEO_NONBLOCK
#error "RTSP_VIDEO_NONBLOCK is not supported by the RFC 2435 packetizer"
#endif

void RTSPServer::rtpVideoTaskWrapper(void* pvParameters) {
  RTSPServer* server = static_cast<RTSPServer*>(pvParameters);
  server->rtpVideoTask();
}

void RTSPServer::rtpVideoTask() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(this->sessionsMutex, portMAX_DELAY) == pdTRUE) {
      for (const auto& sessionPair : this->sessions) {
        const RTSP_Session& session = sessionPair.second;
        if (session.isPlaying) {
          this->sendRtpFrame(this->rtspStreamBuffer,
                             this->rtspStreamBufferSize, this->vQuality,
                             this->vWidth, this->vHeight, session.sock,
                             session.cVideoPort, session.isTCP,
                             session.isMulticast);
          break;
        }
      }
      xSemaphoreGive(this->sessionsMutex);
    }
    this->rtspStreamBufferSize = 0;
    this->rtpFrameSent = true;
  }
  vTaskDelete(NULL);
}

RtpFrameSendResult RTSPServer::sendRTSPFrame(const uint8_t* data, size_t len,
                                             int quality, int width,
                                             int height) {
  RtpFrameSendResult result = {false, 0, ENOTCONN};
  this->rtpFrameSent = false;
  uint32_t currentTime = millis();
  this->videoTimestamp = static_cast<uint32_t>(
    (static_cast<uint64_t>(esp_timer_get_time()) * 9ULL) / 100ULL);

  if (xSemaphoreTake(this->sessionsMutex, portMAX_DELAY) == pdTRUE) {
    for (const auto& sessionPair : this->sessions) {
      const RTSP_Session& session = sessionPair.second;
      if (!session.isPlaying) {
        continue;
      }
      result = sendRtpFrame(data, len, quality, width, height, session.sock,
                            session.cVideoPort, session.isTCP,
                            session.isMulticast);
      break;
    }
    xSemaphoreGive(this->sessionsMutex);
  } else {
    result.error = EBUSY;
  }

  if (result.sent) {
    ++this->rtpFrameCount;
  }
  if (currentTime - this->lastRtpFPSUpdateTime >= 1000) {
    this->rtpFps = this->rtpFrameCount;
    this->rtpFrameCount = 0;
    this->lastRtpFPSUpdateTime = currentTime;
  }
  this->rtpFrameSent = true;
  return result;
}

void RTSPServer::sendRTSPAudio(int16_t* data, size_t len) {
  this->rtpAudioSent = false;
  bool multicastSent = false;
  for (const auto& sessionPair : this->sessions) {
    const RTSP_Session& session = sessionPair.second; 
    if (session.isPlaying) {
      if (session.isMulticast) {
        if (!multicastSent) {
          this->sendRtpAudio(data, len, session.sock, this->rtpAudioPort, false, true);
          multicastSent = true;
        }
      } else {
        this->sendRtpAudio(data, len,  session.isHttp ? session.httpSock : session.sock, session.cAudioPort, session.isTCP, false);
      }
    }
  }
  this->rtpAudioSent = true;
}

void RTSPServer::sendRTSPSubtitles(char* data, size_t len) {
  this->rtpSubtitlesSent = false;
  bool multicastSent = false;
  for (const auto& sessionPair : this->sessions) {
    const RTSP_Session& session = sessionPair.second; 
    if (session.isPlaying) {
      if (session.isMulticast) {
          if (!multicastSent) {
            this->sendRtpSubtitles(data, len, session.sock, this->rtpSubtitlesPort, false, true);
            multicastSent = true;
        }
      } else {
        this->sendRtpSubtitles(data, len,  session.isHttp ? session.httpSock : session.sock, session.cSrtPort, session.isTCP, false);
      }
    }
  }
  this->rtpSubtitlesSent = true;
}

RtpFrameSendResult RTSPServer::sendRtpFrame(
  const uint8_t* data, size_t len, uint8_t quality, uint16_t width,
  uint16_t height, int sock, uint16_t sendRtpPort, bool useTCP,
  bool isMulticast) {
  (void)quality;
  RtpFrameSendResult result = {false, 0, 0};
  if (useTCP || isMulticast) {
    result.error = EPROTONOSUPPORT;
    return result;
  }
  if (this->videoUnicastSocket < 0 || sendRtpPort == 0) {
    result.error = ENOTCONN;
    return result;
  }

  Rfc2435JpegFrame jpegFrame;
  const char *parseError = nullptr;
  if (!parseRfc2435Jpeg(data, len, width, height, &jpegFrame, &parseError)) {
    RTSP_LOGE(LOG_TAG, "RFC 2435 JPEG rejected: %s",
              parseError != nullptr ? parseError : "unknown error");
    result.error = EINVAL;
    return result;
  }
  if (jpegFrame.scan_size > 0x00ffffff) {
    result.error = EFBIG;
    return result;
  }

  struct sockaddr_in clientAddress;
  memset(&clientAddress, 0, sizeof(clientAddress));
  socklen_t addressLength = sizeof(clientAddress);
  if (getpeername(sock, reinterpret_cast<struct sockaddr *>(&clientAddress),
                  &addressLength) == -1) {
    result.error = errno;
    return result;
  }
  clientAddress.sin_port = htons(sendRtpPort);

  static constexpr size_t kMaxDatagramBytes = 1400;
  static constexpr size_t kRtpHeaderBytes = 12;
  static constexpr size_t kJpegHeaderBytes = 8;
  static constexpr size_t kQuantizationHeaderBytes = 4;
  static constexpr size_t kQuantizationTableBytes = 128;
  static constexpr uint32_t kInterPacketPacingUs = 750;
  uint8_t packet[kMaxDatagramBytes];
  size_t fragmentOffset = 0;

  while (fragmentOffset < jpegFrame.scan_size) {
    const bool firstPacket = fragmentOffset == 0;
    size_t headerBytes = kRtpHeaderBytes + kJpegHeaderBytes;
    if (firstPacket) {
      headerBytes += kQuantizationHeaderBytes + kQuantizationTableBytes;
    }
    size_t fragmentBytes = jpegFrame.scan_size - fragmentOffset;
    if (fragmentBytes > kMaxDatagramBytes - headerBytes) {
      fragmentBytes = kMaxDatagramBytes - headerBytes;
    }
    const bool lastPacket = fragmentOffset + fragmentBytes
                            == jpegFrame.scan_size;

    packet[0] = 0x80;
    packet[1] = static_cast<uint8_t>(0x1a | (lastPacket ? 0x80 : 0x00));
    packet[2] = static_cast<uint8_t>(this->videoSequenceNumber >> 8);
    packet[3] = static_cast<uint8_t>(this->videoSequenceNumber);
    packet[4] = static_cast<uint8_t>(this->videoTimestamp >> 24);
    packet[5] = static_cast<uint8_t>(this->videoTimestamp >> 16);
    packet[6] = static_cast<uint8_t>(this->videoTimestamp >> 8);
    packet[7] = static_cast<uint8_t>(this->videoTimestamp);
    packet[8] = static_cast<uint8_t>(this->videoSSRC >> 24);
    packet[9] = static_cast<uint8_t>(this->videoSSRC >> 16);
    packet[10] = static_cast<uint8_t>(this->videoSSRC >> 8);
    packet[11] = static_cast<uint8_t>(this->videoSSRC);

    packet[12] = 0;
    packet[13] = static_cast<uint8_t>(fragmentOffset >> 16);
    packet[14] = static_cast<uint8_t>(fragmentOffset >> 8);
    packet[15] = static_cast<uint8_t>(fragmentOffset);
    packet[16] = 0;
    packet[17] = 255;
    packet[18] = static_cast<uint8_t>(jpegFrame.width / 8);
    packet[19] = static_cast<uint8_t>(jpegFrame.height / 8);

    size_t packetOffset = kRtpHeaderBytes + kJpegHeaderBytes;
    if (firstPacket) {
      packet[packetOffset++] = 0;
      packet[packetOffset++] = 0;
      packet[packetOffset++] = 0;
      packet[packetOffset++] = kQuantizationTableBytes;
      memcpy(packet + packetOffset, jpegFrame.quantization_tables,
             kQuantizationTableBytes);
      packetOffset += kQuantizationTableBytes;
    }
    memcpy(packet + packetOffset, jpegFrame.scan_data + fragmentOffset,
           fragmentBytes);
    packetOffset += fragmentBytes;

    ssize_t sent = sendto(this->videoUnicastSocket, packet, packetOffset, 0,
                          reinterpret_cast<struct sockaddr *>(&clientAddress),
                          sizeof(clientAddress));
    ++this->videoSequenceNumber;
    if (sent != static_cast<ssize_t>(packetOffset)) {
      result.error = sent < 0 ? errno : EIO;
      RTSP_LOGW(LOG_TAG, "RTP/JPEG send failed: errno=%d packets=%u",
                result.error, result.packetCount);
      return result;
    }

    ++result.packetCount;
    fragmentOffset += fragmentBytes;
    if (!lastPacket) {
      delayMicroseconds(kInterPacketPacingUs);
    }
  }

  result.sent = true;
  return result;
}

void RTSPServer::sendRtpAudio(const int16_t* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast) {
  const int RtpHeaderSize = 12; // RTP header size
  const int MAX_FRAGMENT_SIZE = 1446; // Adjust based on your requirements
  uint32_t audioLen = len;

  size_t fragmentOffset = 0;
  while (fragmentOffset < audioLen) {
    int fragmentLen = MAX_FRAGMENT_SIZE;
    if (fragmentLen + fragmentOffset > audioLen) {
      fragmentLen = audioLen - fragmentOffset;
    }

    int RtpPacketSize = fragmentLen + RtpHeaderSize;
    uint8_t packet[2048];
    memset(packet, 0x00, sizeof(packet));

    // If TCP, we need these first 4 bytes
    packet[0] = '$'; // Magic number 
    packet[1] = this->audioCh; // Channel number for RTP (1 for audio)
    packet[2] = (RtpPacketSize >> 8) & 0xFF; // Packet length high byte 
    packet[3] = RtpPacketSize & 0xFF; // Packet length low byte

    // RTP header
    packet[4] = 0x80; // Version: 2, Padding: 0, Extension: 0, CSRC Count: 0
    packet[5] = 0x61 | 0x80;  // Dynamic payload type (97) and marker bit
    packet[6] = (this->audioSequenceNumber >> 8) & 0xFF; // Sequence Number (high byte)
    packet[7] = this->audioSequenceNumber & 0xFF; // Sequence Number (low byte)
    packet[8] = (this->audioTimestamp >> 24) & 0xFF; // Timestamp (high byte)
    packet[9] = (this->audioTimestamp >> 16) & 0xFF; // Timestamp (next byte)
    packet[10] = (this->audioTimestamp >> 8) & 0xFF; // Timestamp (next byte)
    packet[11] = this->audioTimestamp & 0xFF; // Timestamp (low byte)
    packet[12] = (this->audioSSRC >> 24) & 0xFF; // SSRC (high byte)
    packet[13] = (this->audioSSRC >> 16) & 0xFF; // SSRC (next byte)
    packet[14] = (this->audioSSRC >> 8) & 0xFF; // SSRC (next byte)
    packet[15] = this->audioSSRC & 0xFF; // SSRC (low byte)

    int packetOffset = RtpHeaderSize + 4;

    // Convert audio data from little-endian to big-endian and copy to the packet
    for (size_t i = 0; i < fragmentLen / 2; i++) {
      packet[packetOffset++] = (data[fragmentOffset / 2 + i] >> 8) & 0xFF; // High byte
      packet[packetOffset++] = data[fragmentOffset / 2 + i] & 0xFF; // Low byte
    }

    // Send packet using TCP or UDP
    if (useTCP) {
      sendTcpPacket(packet, packetOffset, sock);
    } else {
      struct sockaddr_in client_addr;
      memset(&client_addr, 0, sizeof(client_addr));
      client_addr.sin_family = AF_INET;
      // Determine IP address based on whether it's multicast or unicast
      if (isMulticast) {
        inet_aton(this->rtpIp.toString().c_str(), &client_addr.sin_addr);
      } else {
        socklen_t addrLen = sizeof(client_addr);
        if (getpeername(sock, (struct sockaddr*)&client_addr, &addrLen) == -1) {
          RTSP_LOGE(LOG_TAG, "Failed to get peer IP address");
          return;
        }
      }
      client_addr.sin_port = htons(sendRtpPort);

      int rtpSocket = isMulticast ? this->audioMulticastSocket : this->audioUnicastSocket;

      sendto(rtpSocket, packet + 4, packetOffset - 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
    }
    fragmentOffset += fragmentLen;
    this->audioSequenceNumber++;
    this->audioTimestamp += fragmentLen / 2; // Convert fragment length to number of samples
  }
}

void RTSPServer::sendRtpSubtitles(const char* data, size_t len, int sock, uint16_t sendRtpPort, bool useTCP, bool isMulticast) {
  const int RtpHeaderSize = 12; // RTP header size
  int RtpPacketSize = len + RtpHeaderSize;

  uint8_t packet[512];
  memset(packet, 0x00, sizeof(packet));

  // If TCP, we need these first 4 bytes
  packet[0] = '$'; // Magic number 
  packet[1] = this->subtitlesCh; // Channel number for RTP (2 for subtitles)
  packet[2] = (RtpPacketSize >> 8) & 0xFF; // Packet length high byte 
  packet[3] = RtpPacketSize & 0xFF; // Packet length low byte
  
  // RTP header
  packet[4] = 0x80; // Version: 2, Padding: 0, Extension: 0, CSRC Count: 0
  packet[5] = 0x80 | 0x62; // Marker bit set and payload type 98
  packet[6] = (this->subtitlesSequenceNumber >> 8) & 0xFF; // Sequence Number (high byte)
  packet[7] = this->subtitlesSequenceNumber & 0xFF; // Sequence Number (low byte)
  packet[8] = (this->subtitlesTimestamp >> 24) & 0xFF; // Timestamp (high byte)
  packet[9] = (this->subtitlesTimestamp >> 16) & 0xFF; // Timestamp (next byte)
  packet[10] = (this->subtitlesTimestamp >> 8) & 0xFF; // Timestamp (next byte)
  packet[11] = this->subtitlesTimestamp & 0xFF; // Timestamp (low byte)
  packet[12] = (this->subtitlesSSRC >> 24) & 0xFF; // SSRC (high byte)
  packet[13] = (this->subtitlesSSRC >> 16) & 0xFF; // SSRC (next byte)
  packet[14] = (this->subtitlesSSRC >> 8) & 0xFF; // SSRC (next byte)
  packet[15] = this->subtitlesSSRC & 0xFF; // SSRC (low byte)

  int packetOffset = RtpHeaderSize + 4;

  // Copy SRT data to the packet
  memcpy(packet + packetOffset, data, len);
  packetOffset += len;

  // Send packet using TCP or UDP
  if (useTCP) {
    sendTcpPacket(packet, packetOffset, sock);
  } else {
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    // Determine IP address based on whether it's multicast or unicast
    if (isMulticast) {
      inet_aton(this->rtpIp.toString().c_str(), &client_addr.sin_addr);
    } else {
      socklen_t addrLen = sizeof(client_addr);
      if (getpeername(sock, (struct sockaddr*)&client_addr, &addrLen) == -1) {
        RTSP_LOGE(LOG_TAG, "Failed to get peer IP address");
        return;
      }
    }
    client_addr.sin_port = htons(sendRtpPort);  

    int rtpSocket = isMulticast ? this->subtitlesMulticastSocket : this->subtitlesUnicastSocket;

    sendto(rtpSocket, packet + 4, packetOffset - 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
  }
  this->subtitlesSequenceNumber++;
  this->subtitlesTimestamp += 1000; // Increment the timestamp
}
