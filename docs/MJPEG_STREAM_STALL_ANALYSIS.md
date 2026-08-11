# MJPEG Stream Stall Analysis - Handoff

Status: unresolved. Root cause narrowed to the TCP send path. One remediation
attempt was tested on hardware and failed.

## 1. Symptom

The ESP32-P4 serves an HTTP MJPEG stream at `192.168.1.140:80`, viewed in a
browser. The video stalls roughly every 5 seconds. Each stall lasts about 1 to
2 seconds, then the stream resumes normally.

## 2. System Under Test

| Item | Value |
| --- | --- |
| Board | ESP32-P4 CB V1.3 |
| Sensor | OV5647 on MIPI-CSI |
| Wi-Fi | ESP32-C6 coprocessor over ESP-Hosted, SDIO 4-bit, 40 MHz |
| Arduino core | esp32 3.3.11 |
| Capture format | RGB565, 800x800, 2 buffers |
| Encoder | ESP32-P4 hardware JPEG peripheral, quality 80, YUV422 subsampling |
| Stream rate | 200 ms interval (5 FPS target), set by `kJpegIntervalMs` |
| Server | `esp_http_server`, single synchronous handler, `httpd_resp_send_chunk` |
| Sketch | `mipi_csi_camera/mipi_csi_camera.ino` |

The handler runs capture, encode, and socket write sequentially in one task.
Encoded frames are approximately 70-74 KB.

## 3. Measured Data

Instrumented log, sequences 50-100. Fields are per-frame stage timings in
microseconds. Full log is in the project history; representative lines:

```
mjpeg sequence=78 bytes=69850 capture_us=12 encode_us=8609 write_us=65418    cycle_us=74061
mjpeg sequence=79 bytes=71164 capture_us=14 encode_us=8574 write_us=1529232  cycle_us=1537841
mjpeg sequence=80 bytes=70550 capture_us=12 encode_us=8564 write_us=44602    cycle_us=53199
...
mjpeg sequence=91 bytes=71912 capture_us=12 encode_us=8584 write_us=21489    cycle_us=30106
mjpeg sequence=92 bytes=71909 capture_us=12 encode_us=8602 write_us=1346613  cycle_us=1355249
mjpeg sequence=93 bytes=70916 capture_us=12 encode_us=8603 write_us=60488    cycle_us=69124
```

Aggregate over the 51-sample window:

| Stage | Min | Max | Behaviour |
| --- | --- | --- | --- |
| `capture_us` | 12 | 15 | Constant. Never varies. |
| `encode_us` | 8543 | 8627 | Constant, ~8.58 ms. Never varies. |
| `write_us` | 10778 | 1529232 | **Entire variance lives here.** |

Two stalls in the window:

- seq 79: `write_us` = 1,529,232 (1.53 s)
- seq 92: `write_us` = 1,346,613 (1.35 s)

They are 13 frames apart. At the 200 ms frame interval that is ~2.6 s of
stream time, consistent with the observed stall cadence.

## 4. What This Rules Out

The camera and the JPEG encoder are not involved. `capture_us` and
`encode_us` are flat across every sample, including the two stall frames. The
MIPI-CSI capture path and the hardware JPEG peripheral are behaving correctly
and are not the bottleneck. Any future investigation should stay in the
network path.

Raw bandwidth is also not the constraint:

- Average load: 72 KB per frame at 5 FPS = **~2.9 Mbps**
- Fast frames: 72 KB in 10.8 ms = **~53 Mbps** instantaneous
- Typical frames: 72 KB in 20-30 ms = **~19-28 Mbps** instantaneous

The link has ample headroom on average. The traffic shape is the problem, not
the volume: each frame is emitted as one full-line-rate burst, and the stream
is idle in between.

## 5. Root Cause Assessment

A 1.3-1.5 s block inside a blocking TCP write is characteristic of a
**retransmission timeout (RTO)** rather than ordinary congestion or a slow
link. Congestion produces gradual, proportional slowdown; the observed
behaviour is a bimodal jump from tens of milliseconds to over a second, with
nothing in between.

Relevant SDK configuration, read from
`~/Library/Arduino15/packages/esp32/tools/esp32p4-libs/3.3.11/sdkconfig`:

| Setting | Value | Relevance |
| --- | --- | --- |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 65534 | Smaller than one ~72 KB JPEG frame |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 65534 | Same ceiling on the receive window |
| `CONFIG_LWIP_TCP_MSS` | 1436 | A 72 KB frame is ~50 segments |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 32 | ~50 segments queued against 32 buffers |
| `CONFIG_LWIP_TCP_RTO_TIME` | 3000 | Each RTO recovery event costs seconds |
| `CONFIG_LWIP_TCP_SACK_OUT` | y | Selective ACK is enabled |
| `CONFIG_LWIP_TCPIP_TASK_AFFINITY` | CPU0 | lwIP task pinned to core 0 |
| `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` | 4096 | |

Proposed mechanism, stated as a hypothesis and not yet proven:

1. Each frame is written as a single ~72 KB `httpd_resp_send_chunk` call, which
   exceeds the 65534-byte send buffer. The write blocks until space frees.
2. The frame expands to ~50 MTU-sized segments handed to a 32-buffer Wi-Fi TX
   pool in one burst, so buffer exhaustion is likely at the radio layer.
3. When a segment is lost from that burst, fast retransmit needs duplicate ACKs
   to trigger. With the send buffer already saturated there is limited room to
   generate and process them promptly.
4. Recovery therefore falls back to the RTO timer, producing the observed
   1.3-1.5 s block.

Note that step 3 is the weakest link in the chain and is the part most worth
verifying directly, since `CONFIG_LWIP_TCP_SACK_OUT` is enabled and should
normally assist recovery.

A contributing factor was also identified: the Arduino core defaults Wi-Fi
modem power save to `WIFI_PS_MIN_MODEM`
(`libraries/WiFi/src/WiFiGeneric.cpp:388`), applied when the `STA_START` event
fires (`libraries/WiFi/src/STA.cpp:122`). The sketch never overrides it, so the
C6 radio sleeps between beacons. This adds latency to bursts and to inbound
ACKs. It is unlikely to be the sole cause of a 1.3 s stall on its own.

## 6. Remediation Attempted and Rejected

A change set was written, compiled clean (44% flash, 9% RAM), and tested on
hardware. **The stalls persisted.** The code has been fully reverted; the
sketch is back to its pre-attempt state.

What was tried, all at once:

- Splitting each frame into 8 KB `httpd_resp_send_chunk` calls instead of one
  72 KB call
- `WiFi.setSleep(WIFI_PS_NONE)` before `WiFi.STA.begin()`
- `TCP_NODELAY` on the client socket
- `SO_SNDTIMEO` at 800 ms, below the 3 s RTO
- HTTP server task pinned to core 1, away from the lwIP task on core 0
- `lru_purge_enable` for stale sessions on page reload

Because all six changes were applied together and the result was still bad,
**no individual change has been isolated as ineffective.** The slicing approach
was judged unhelpful and is not being pursued further, but note that slicing
addresses buffer occupancy, not packet loss. If frames are being lost on the
radio link, smaller writes would not prevent the loss, only change how it
recovers. That the symptom survived does not by itself refute the RTO
hypothesis.

## 7. Preferred Next Direction: Larger TCP Buffers

The stated preference is to increase the TCP buffer size rather than slice
writes. Two hard constraints apply, both verified against the installed SDK:

**65534 is the protocol ceiling, not an SDK default.** The TCP window field is
16 bits wide, so 65535 is the maximum advertisable window without the window
scaling option (RFC 7323). Going beyond it requires `CONFIG_LWIP_WND_SCALE`,
which is **absent from this sdkconfig** (verified: no `WND_SCALE` or
`TCP_RCV_SCALE` entry exists). Window scaling must be enabled before any value
above 65534 has meaning.

**The Arduino core ships lwIP as a prebuilt binary.** There are 136 precompiled
`.a` archives in
`~/Library/Arduino15/packages/esp32/tools/esp32p4-libs/3.3.11/lib/`, including
`liblwip.a`. Editing `sdkconfig` in that directory has no effect, because
nothing recompiles it. Changing any `CONFIG_LWIP_*` value requires building the
project under ESP-IDF from source, or rebuilding the Arduino core libraries via
`esp32-arduino-lib-builder`.

So the buffer-size route means moving off the stock Arduino toolchain. That is a
real cost and should be a deliberate decision.

## 8. Recommended Investigation Order

Diagnose before changing more code. The single most valuable next step is a
**packet capture** (Wireshark in monitor mode, or `tcpdump` on the viewing
machine) taken across a stall. This settles the root cause definitively rather
than by inference. Look for:

- TCP retransmissions and their timing relative to the stall
- Whether duplicate ACKs precede the stall (fast retransmit attempted) or are
  absent (straight to RTO)
- Zero-window advertisements from the browser, which would move the cause to
  the client side rather than the ESP32
- 802.11 layer retries, if a monitor-mode capture is available

Then, in rough order of cost:

1. **Isolate the power-save variable alone.** Apply only
   `WiFi.setSleep(WIFI_PS_NONE)` before `WiFi.STA.begin()` and re-measure. One
   line, no toolchain change, and it was never tested in isolation.
2. **Test a different client.** `tools/pc_receiver.py` already exists and uses a
   raw TCP framing protocol. Stage 6 of
   `OV5647_INCREMENTAL_JPEG_STREAM_PLAN.md` records that path sustaining 9.991
   FPS over 50 seconds with no stall. If the raw-TCP path is still clean while
   the browser path stalls, the fault is in the HTTP or browser layer, not in
   Wi-Fi or lwIP. **This is a strong existing data point and should be
   re-confirmed early**, since it can eliminate the entire lower stack.
3. **Reduce burst size at the source.** Lowering JPEG quality or resolution
   shrinks each frame below the 65534-byte send buffer. This tests the
   oversized-burst hypothesis directly without touching the toolchain. It
   changes image quality, so it is a diagnostic step, not necessarily the final
   configuration.
4. **Decouple capture from transmission.** A frame queue with the encoder and
   the socket writer in separate tasks stops a blocked write from stalling
   capture. This is already scoped as Stage 8 (Dual-Core Separation) in the
   incremental plan. It improves stall *tolerance* and would likely smooth the
   symptom, but does not remove the underlying packet loss or RTO.
5. **Raise the buffers.** Move to ESP-IDF or rebuild the core libraries, enable
   `CONFIG_LWIP_WND_SCALE`, raise `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and
   `CONFIG_LWIP_TCP_WND_DEFAULT`, and consider raising
   `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` from 32 so the TX pool can absorb a
   full frame burst. Also consider lowering `CONFIG_LWIP_TCP_RTO_TIME` from
   3000 so that any RTO that does occur costs far less. Highest effort, and
   worth doing after the packet capture has confirmed the mechanism.

## 9. Reference Points

- Sketch under test: `mipi_csi_camera/mipi_csi_camera.ino`, handler
  `streamHandler`
- Encoder wrapper: `mipi_csi_camera/jpeg_encoder.cpp`, note
  `engineConfig.timeout_ms = 40`
- Staged bring-up history and prior baselines:
  `docs/OV5647_INCREMENTAL_JPEG_STREAM_PLAN.md`
- Upstream reference implementation:
  `reference/ESP32-CAM-Video-Streaming.ino` (ESP32-CAM, `esp_camera`, native
  JPEG from sensor; note it sends the boundary *after* each frame while this
  sketch sends it before, and it has no rate limiter)
- PC-side raw TCP receiver: `tools/pc_receiver.py`
- SDK config reference:
  `~/Library/Arduino15/packages/esp32/tools/esp32p4-libs/3.3.11/sdkconfig`

## 10. Open Questions

- Does a packet capture show duplicate ACKs before the stall, or a jump
  straight to RTO?
- Does the raw TCP path (`pc_receiver.py`) still run stall-free on the current
  build? If yes, why does the HTTP path differ?
- Does `WIFI_PS_NONE` alone change the stall frequency or duration?
- Is the 5 s stall cadence tied to a beacon interval, a DHCP or ARP refresh, or
  another periodic network event? The ~2.6 s spacing measured between seq 79
  and 92 does not obviously match the reported ~5 s, which suggests stall
  frequency may be variable and warrants a longer capture window.
