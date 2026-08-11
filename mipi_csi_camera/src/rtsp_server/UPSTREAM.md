# Vendored ESP32-RTSPServer

- Upstream: <https://github.com/rjsachse/ESP32-RTSPServer>
- Release: `1.3.5`
- Upstream commit: `2bd79ca6e9e6d8a0e6651735cd7a604746106e2a`
- Release archive SHA-256: `10693ef8d1dacae0b1202a4115de9b8edf9bac70d39b548fc7b330dcf0a81898`
- License: MIT; see [LICENSE](LICENSE)

## Local changes

- Limit the prototype to one unicast RTP/JPEG client over UDP.
- Negotiate the client RTP and RTCP ports from RTSP `SETUP`.
- Bind local RTP/RTCP ports `5430-5431` and discard incoming RTCP reports.
- Advertise the actual board address and configured RTSP port.
- Reject multicast and RTP-over-TCP with RTSP status 461.
- Close session and media sockets after teardown or control-socket loss.
- Protect session state shared by the control and frame-sending tasks.
- Replace the upstream full-JPEG payload with an RFC 2435 packetizer.
- Validate baseline 800x800 YUV422 JPEG structure and standard Huffman tables.
- Send `Q=255` with both JPEG quantization tables in the first RTP packet.
- Cap UDP datagrams at 1400 bytes and abandon a frame on a send error.
- Pace UDP fragments 750 us apart to avoid a short burst at the Wi-Fi bridge.
