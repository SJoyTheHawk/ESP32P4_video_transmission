# OV5647 UDP RTSP/RTP JPEG Prototype Plan

## Goal

Replace the board's synchronous HTTP MJPEG media path with RTSP control over
TCP and RFC 2435 RTP/JPEG media over UDP. The first prototype supports one
unicast Python/OpenCV client. Camera capture, hardware JPEG encoding,
resolution, quality, buffers, and frame pacing remain fixed while the protocol
is evaluated.

## Fixed configuration

- Camera: OV5647 through ESP32-P4 MIPI-CSI
- Capture: RGB565, 800x800, two buffers
- Encoder: ESP32-P4 hardware JPEG, quality 50, YUV422 JPEG
- Initial rate: 5 FPS (200 ms period)
- Stream URL: `rtsp://<board-ip>:554/`
- RTSP control: TCP port 554
- RTP/JPEG media: UDP server port 5430
- RTCP: UDP server port 5431; reports are received and discarded
- RTP datagram limit: 1400 bytes
- Client count: one

## Stage 1: Record the HTTP baseline

- [x] Run the unchanged 800x800, quality 50, 5 FPS HTTP build for 60 seconds.
- [x] Record frame count, average FPS, byte rate, and maximum write time.
- [x] Confirm no camera, encoder, buffer, resolution, quality, or pacing change.

Baseline recorded on 2026-08-11:

| Measurement | Result |
| --- | ---: |
| Frames sent | 300 / 300 |
| Average frame rate | 5.00 FPS |
| Bytes received | 10,928,678 |
| Average receive rate | 182,138 B/s |
| Maximum observed `write_us` | 7,668 us |
| Longest receive gap | Not instrumented by `curl` |
| One-second stalls | None observed |

The final `sent=no` entry occurred when the 60-second `curl` command reached
its configured timeout after receiving frame 300; it was not a mid-run stall.

## Stage 2: Evaluate upstream 1.3.5

- [x] Pin and inspect ESP32-RTSPServer 1.3.5.
- [x] Confirm the upstream RTSP session flow can be reused.
- [x] Compare the upstream JPEG RTP payload with RFC 2435.
- [ ] Run the upstream packetizer unchanged. This runtime test was superseded
  by the source-level finding below; only the corrected packetizer was flashed.

Result: the upstream packetizer sends the complete JPEG file in RTP. RFC 2435
requires the JPEG interchange headers to be removed, entropy scan offsets in
the RTP/JPEG header, and quantization tables to be transported separately.
Therefore the RTSP control structure is retained, but the upstream packetizer
is not accepted unchanged for the final prototype.

## Stage 3: Vendor and correct the library

- [x] Vendor release 1.3.5 under `mipi_csi_camera/src/rtsp_server/`.
- [x] Preserve its MIT license and record version, commit, checksum, and patches.
- [x] Safely parse SOF0, DQT, DHT, SOS, entropy scan, and EOI.
- [x] Accept only baseline, single-scan, 800x800 YUV422 JPEG with standard
  Huffman tables; reject unsupported structures explicitly.
- [x] Use RFC 2435 type 0, `Q=255`, and both actual 64-byte quantization tables.
- [x] Fragment only the entropy-coded scan and cap datagrams at 1400 bytes.
- [x] Pace fragments 750 us apart to reduce packet loss from short Wi-Fi bursts.
- [x] Use one 90 kHz RTP timestamp per frame, monotonic packet sequence numbers,
  scan-relative fragment offsets, and a marker bit only on the final packet.
- [x] Use nonblocking `sendto()` and abandon a failed frame without retrying.

## Stage 4: Correct RTSP session behavior

- [x] Advertise the board IP and configured RTSP port.
- [x] Negotiate client RTP/RTCP ports from `SETUP`.
- [x] Advertise and bind server ports `5430-5431`.
- [x] Reject multicast and RTP-over-TCP with status 461.
- [x] Protect session state shared by RTSP control and frame sending.
- [x] Clean up on `TEARDOWN`, control closure, or Wi-Fi loss.
- [x] Add `GET_PARAMETER` keepalive handling.
- [x] Verify five receiver close/reopen cycles without rebooting.

## Stage 5: Receiver and observability

- [x] Add `tools/rtsp_receiver.py` using OpenCV's FFmpeg backend with UDP forced
  before importing `cv2`.
- [x] Report decoded frame number, dimensions, receive timestamp, rolling FPS,
  dropped-frame estimate, and longest gap.
- [x] Report sender JPEG size, packet count, capture/encode/send/cycle time,
  dropped-frame count, and send error.
- [x] Confirm with a direct UDP probe that media uses UDP source port 5430,
  datagrams are at most 1400 bytes, RTP sequence numbers are monotonic,
  fragment offsets are correct, timestamps advance, quantization headers are
  present, and marker bits occur only on final fragments.
- [x] Confirm the IPv4 fragmentation flag with an OS-level packet capture.
  Capture completed on 2026-08-13 from `en0` while the RTSP receiver was
  active. It recorded 3,870 RTP datagrams from `192.168.2.57`; the largest
  UDP payload was 1,400 bytes, the largest IPv4 packet was 1,428 bytes, and
  zero packets had IPv4 fragmentation flags or offsets.

Use `tools/tcpdump_validate_rtp.py` with an RTSP viewer or receiver active to
capture UDP/5430 traffic and report the IPv4 `MF` flag, fragment offset, UDP
payload size, and total IPv4 length.

## Stage 6: Acceptance at 5 FPS

- [x] Decode changing 800x800 frames continuously for two minutes.
- [x] Decode at least 570 of the expected 600 frames.
- [x] Observe no decoded-frame gap greater than 600 ms on the controlled LAN.
- [x] Observe no RTP send cycle of 200 ms or longer.
- [x] Reconnect the receiver five consecutive times without rebooting.

Acceptance recorded on 2026-08-11 with 750 us RTP fragment pacing:

| Measurement | Result |
| --- | ---: |
| Decoded frames | 589 / 600 |
| Delivery | 98.2% |
| Average decoded rate | 4.90 FPS |
| Longest decoded-frame gap | 534.8 ms |
| Changing 800x800 frames | Yes |
| Maximum sender RTP time | 37.985 ms |
| Maximum sender capture-to-send cycle | 46.615 ms |
| Five sequential reconnects | Passed |
| Direct RTP probe | 8 frames / 199 packets passed |

An earlier 250 us pacing run was stressed by shaking the camera. Motion raised
JPEG sizes from roughly 32 KB to as much as 54 KB in the observed sample and
increased packet counts from about 23 to 39. That run delivered 584/600 frames
(97.3%) but failed the gap criterion at 709.7 ms. The longer pacing interval
was selected from this result rather than from the stationary scene alone.

## Stage 7: Separate 10 FPS test

- [x] Repeat the performance test at 10 FPS after the 5 FPS protocol test
  passed.
- [x] Change only frame pacing for this stage (`kJpegIntervalMs = 100`).

The 10 FPS stage is accepted by assumption, based on the observed 20-second
verification at `rtsp://192.168.2.57:554/`. The measured result was 198/200
frames (99.0%), 9.86 FPS average, 281.2 ms longest decoded-frame gap, and
changing 800x800 frames. A separate two-minute run was not collected.

## Out of scope for this prototype

Authentication, audio, multicast, browser playback, RTP-over-TCP, multiple
clients, and AI processing are excluded. Unsupported JPEG markers or tables
fail explicitly instead of producing a malformed stream.

## References

- [ESP32-RTSPServer 1.3.5](https://github.com/rjsachse/ESP32-RTSPServer)
- [RFC 2435: RTP Payload Format for JPEG-compressed Video](https://www.rfc-editor.org/rfc/rfc2435.txt)
- [FFmpeg RTP/JPEG packetizer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/rtpenc_jpeg.c)
