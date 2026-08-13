# ESP32P4 Video Transmission

- Board: ESP32-P4 CB V1.3
- Camera: OV5647 MIPI-CSI
- Wireless: ESP32-C6 via ESP-Hosted SDIO
- Capture and hardware JPEG encoding: validated
- RTSP control with UDP RTP/JPEG media: validated at 5 FPS and accepted at 10 FPS

## Current Prototype

The ESP32 hosts a single-client RTSP server. RTSP control uses TCP port 554,
RFC 2435 RTP/JPEG media uses UDP port 5430, and RTCP reports are accepted on
UDP port 5431. The stream is 800x800, JPEG quality 50, and currently 10 FPS.

```text
rtsp://<board-ip>:554/
```

The receiver requests unicast UDP media; multicast, RTP-over-TCP, audio,
authentication, browser playback, and multiple clients are outside this
prototype.

## Validation

The 5 FPS acceptance run decoded 589/600 changing frames over two minutes
(98.2%), with a 534.8 ms longest gap. The 10 FPS check decoded 198/200 frames
(99.0%) over 20 seconds, with a 281.2 ms longest gap. Five sequential receiver
reconnects passed without rebooting the ESP32. The largest observed RTP send time was
37.985 ms and the largest capture-to-send cycle was 46.615 ms. An OS-level
packet capture recorded 3,870 RTP datagrams with a maximum 1,400-byte UDP
payload, 1,428-byte IPv4 packet, and zero fragmented packets.

```bash
python3 tools/rtsp_viewer.py rtsp://192.168.2.57:554/
python3 tools/rtsp_receiver.py rtsp://192.168.2.57:554/
python3 tools/rtp_packet_probe.py rtsp://192.168.2.57:554/
```

To validate IPv4 fragmentation, start a viewer or receiver in one terminal,
then run the capture utility in another terminal. Packet capture may require
administrator access on macOS:

```bash
sudo python3 tools/tcpdump_validate_rtp.py \
  --host 192.168.2.57 --interface en0 --duration 30
```

The utility writes `tools/rtp_capture.pcap` (ignored by Git), reports the
largest UDP and IPv4 packet sizes, and passes only when no captured packet has
the IPv4 `MF` flag or a nonzero fragment offset.

## Files

- Firmware: `mipi_csi_camera/mipi_csi_camera.ino`
- OpenCV stream viewer: `tools/rtsp_viewer.py`
- OpenCV receiver: `tools/rtsp_receiver.py`
- RTP packet probe: `tools/rtp_packet_probe.py`
- tcpdump fragmentation validator: `tools/tcpdump_validate_rtp.py`
- RTSP/RTP plan and results: `docs/OV5647_UDP_RTSP_RTP_JPEG_PLAN.md`
- Vendored library provenance: `mipi_csi_camera/src/rtsp_server/UPSTREAM.md`
- Historical TCP receiver: `tools/pc_receiver.py`
