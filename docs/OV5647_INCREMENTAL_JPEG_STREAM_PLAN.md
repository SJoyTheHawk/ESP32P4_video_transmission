# Incremental OV5647 JPEG Stream Bring-Up Plan

## Objective

Build a small Arduino test program that:

1. Captures OV5647 frames through MIPI-CSI.
2. Compresses selected RGB565 frames with the ESP32-P4 JPEG encoder.
3. Sends consecutive JPEG images over Wi-Fi.
4. Displays the latest images on the PC as a continuous JPEG video stream.

This is hardware bring-up firmware, not production firmware. Each stage adds one
observable behavior. The next stage starts only after the current firmware has
been flashed to the board and its result has been recorded.

This document supersedes Step 3 of `OV5647_CAPTURE_VALIDATION_PLAN.md`. The
earlier per-frame HTTP design must not be reused for the JPEG stream bring-up.

## Development Rules

- Begin every stage from the last firmware that passed on hardware.
- Make one functional change per stage.
- Keep the active path short and synchronous until its timing is understood.
- Log only measurements needed to accept or reject the current stage.
- Do not add recovery logic, checksums, disk recording, remote control, or
  generalized abstractions during initial bring-up.
- Do not add a queue or another task until the sequential path has passed.
- Do not use one HTTP request per JPEG. The final stream uses one persistent TCP
  connection.
- When a stage fails, stop there. Do not compensate by adding buffers, tasks, or
  retries before identifying the first changed timing value.
- Commit or otherwise identify every firmware image that is flashed, so a known
  passing image can be rebuilt exactly.

## Fixed Hardware Configuration

| Item | Configuration |
| --- | --- |
| Target | ESP32-P4 CB V1.3 |
| Sensor | OV5647, MIPI-CSI two-lane |
| SCCB | I2C port 0, SCL GPIO29, SDA GPIO28 |
| Sensor reset | GPIO26 |
| Sensor power-down | GPIO27 |
| Capture device | `/dev/video0` |
| Initial mode | 800x800, sensor-defined 50 FPS |
| Capture format | RGB565 |
| Capture buffers | 2 |
| Wi-Fi controller | ESP32-C6 through ESP-Hosted SDIO |
| SDIO | D0-D3 GPIO49-52, CMD GPIO53, CLK GPIO54 |
| C6 wake/enable | GPIO12 / GPIO19 |

Do not change resolution, capture format, buffer count, JPEG quality, or stream
rate in the same board test.

## Stage 0 - Repository Rollback

Status: complete in the working tree.

- Restore the tracked sketch, receiver, build configuration, README, and
  validation document to the repository versions.
- Remove the experimental camera pipeline and JPEG stream classes.
- Preserve reference programs, board pinout files, and captured test images.

Acceptance:

- `mipi_csi_camera` builds using the repository Arduino configuration.
- No experimental pipeline source is part of the sketch.

## Stage 1 - Minimal Camera Capture

Status: passed on hardware. ESP-Hosted, Wi-Fi association, and the PC health
request were compiled out. The accepted camera-only baseline is 35.74 FPS at
800x800 RGB565.

Purpose: reproduce the known OV5647 capture rate without Wi-Fi or JPEG activity.

Implementation:

- Start from the Espressif `ESP_Video` camera example.
- Keep only camera pin setup, CSI initialization, `/dev/video0`, RGB565 format,
  two capture buffers, and the capture loop.
- Do not initialize ESP-Hosted or Wi-Fi.
- Do not copy frame data.
- Keep the repository's existing sampled frame hash unchanged.
- Allow `ESPVideoBufferClass` destruction to return each buffer immediately.
- Count valid frames and print one aggregate line every 50 frames:

```text
capture frames=500 width=800 height=800 bytes=1280000 interval_us=... fps=... invalid=0
```

Board test:

1. Flash the firmware.
2. Run for at least 60 seconds.
3. Save several consecutive aggregate lines.

Acceptance:

- Dimensions are consistently 800x800.
- Payload size is consistently 1,280,000 bytes.
- No invalid buffers occur.
- Capture is stable at the recorded 35.74 FPS baseline.

The difference from the sensor's nominal 50 FPS remains recorded, but it does
not block the isolated JPEG timing test.

## Stage 2 - JPEG Encode Without Wi-Fi

Status: passed on hardware at 1, 5, and 10 JPEG FPS settings with quality 80.
The camera remained stable at 35.74 FPS, all encoded JPEGs were valid, encoding
time was stable, and no invalid camera frames were reported.

Purpose: validate the encoder independently from networking.

Build requirement: select `Tools > PSRAM > Enabled`. The JPEG encoder output
buffer is sized for one 800x800 RGB565 frame and requires PSRAM.

Implementation:

- Add a small JPEG encoder class in separate `.h` and `.cpp` files.
- The class owns only encoder initialization and its JPEG output buffer.
- Expose one operation that accepts the current RGB565 pointer and dimensions
  and returns the encoded pointer, size, and elapsed encoding time.
- Encode directly from the valid camera buffer; do not add a 1.28 MB staging
  copy in this stage.
- Keep capture and encoding sequential in the Arduino loop.
- Initially encode one frame per second and discard the JPEG after checking its
  SOI and EOI markers.
- Log one line per encoded frame:

```text
jpeg sequence=... bytes=... encode_us=... valid=yes
```

Board test order:

1. Burn and test at 1 JPEG FPS.
2. If it passes, change only the rate to 5 JPEG FPS and burn again.
3. If it passes, change only the rate to 10 JPEG FPS and burn again.

Acceptance:

- No JPEG encode failures.
- Every result has valid JPEG start/end markers.
- Encode time and encoded size are stable.
- Camera capture rate remains explainable from the measured encode time.

## Stage 3 - Wi-Fi Initialization Only

Status: passed on hardware. Wi-Fi associated successfully while capture remained
at 35.71-35.74 FPS, JPEG encoding remained at 8.54-8.56 ms, all JPEGs were
valid, and no invalid camera frames were reported.

Purpose: identify whether ESP-Hosted or Wi-Fi activity changes camera timing.

Implementation:

- Add only the known board SDIO pins, C6 wake/enable signals, ESP-Hosted
  initialization, and station association.
- Do not open a socket or send a JPEG.
- Keep Stage 2 capture and JPEG behavior unchanged.
- Log Wi-Fi connection state once, then retain the same capture/JPEG logs.

Acceptance:

- Wi-Fi associates successfully.
- Any change in capture FPS or encode time is measured and recorded.
- If camera timing regresses, stop and investigate ESP-Hosted task, interrupt,
  and memory-bandwidth interaction before adding transport.

## Stage 4 - One JPEG Over Persistent TCP

Status: passed on hardware. The ESP32 sender at 192.168.1.91 and the PC receiver
both reported sequence 1 as 16,133 bytes with valid JPEG markers. The TCP write
took 2.302 ms. Dedicated TCP port 5001 is used because an older local HTTP
client still reaches port 8080.

Purpose: validate the transport without per-frame HTTP setup or acknowledgements.

### PC Receiver

- Implement a TCP listener using the Python standard library.
- Read a fixed-size binary header followed by exactly one JPEG payload.
- Validate only header length and JPEG SOI/EOI markers.
- Keep the newest JPEG in memory.
- Do not calculate CRC32 or SHA-256.
- Do not write frames to disk.
- Print receive sequence, byte count, and receive time.

Initial frame header:

| Field | Type |
| --- | --- |
| Magic `JPG0` | 4 bytes |
| Sequence | unsigned 32-bit, network byte order |
| Width | unsigned 16-bit, network byte order |
| Height | unsigned 16-bit, network byte order |
| JPEG length | unsigned 32-bit, network byte order |

### Arduino Sender

- Open one `WiFiClient` connection during setup.
- Send the header and one JPEG with complete-write loops.
- Keep the connection open after the frame.
- Do not wait for a per-frame response.
- Send one frame, then stop sending while capture continues.

Acceptance:

- The receiver obtains exactly one valid JPEG.
- The reported sequence, dimensions, and length match the sender.
- The JPEG can be opened on the PC.

## Stage 5 - Low-Rate Continuous JPEG Stream

Status: passed on hardware at one JPEG per second. The stream ran through at
least sequence 154, exceeding two minutes, with consecutive sequences and valid
JPEGs. Sender and receiver both reported sequence 5 as 85,828 bytes. Encoding
took 8.618 ms and the TCP write took 67.307 ms for that frame, with no
unexplained second-scale pauses. Reconnect behavior remains deferred, so the
receiver must be running before the board starts.

Purpose: prove framing and persistent connection behavior over multiple images.

Implementation:

- Send one JPEG per second over the existing TCP connection.
- Increment the sequence for each JPEG.
- On the receiver, replace the in-memory latest frame after each complete JPEG.
- Measure encoding time, socket write time, total cycle time, and received FPS.
- Add no reconnect handling yet. A broken connection ends the test visibly.

Acceptance:

- Run continuously for at least two minutes.
- Sequences are consecutive.
- Sender and receiver byte counts match.
- No frame takes an unexplained second-scale pause.

## Stage 6 - Increase Stream Rate

Status: passed on hardware at 5 and 10 JPEG FPS with quality 80. Camera capture
remained stable at 35.74 FPS with no invalid frames, JPEG encoding remained near
8.55 ms, and sender/receiver byte counts matched. At the 10 FPS setting, the PC
received 500 sequence increments in 50.047177 seconds, or 9.991 FPS, without a
growing backlog.

Change only the requested JPEG rate between firmware burns:

1. Test 5 FPS.
2. Test 10 FPS.

For each rate, record:

- Camera FPS.
- JPEG encode time and size.
- TCP write time.
- Receiver FPS.
- Sequence gaps.

Use a moderate fixed JPEG quality for these tests. Do not test quality 100 and
rate changes simultaneously. If socket write time exceeds the frame period,
stop and measure link throughput before changing task structure.

Acceptance:

- Receiver FPS follows the configured rate without a growing backlog.
- Capture, encode, and send timing explain the complete cycle.

### Minimal Onboard Server Baseline

Status: ready for hardware validation. The current test temporarily replaces
the outbound `JPG0` connection with one synchronous HTTP MJPEG handler at
`http://<board-ip>/`. The handler captures, encodes, and writes each frame in
sequence using the existing single JPEG output buffer. It has no handoff queue,
extra task, or Python server dependency.

Acceptance:

- One browser or Python client receives changing, valid JPEG frames.
- Serial output reports encode and HTTP write time for every sequence.
- Closing the client ends the handler without resetting the board.

## Stage 7 - PC Live Preview

Status: awaiting a hardware retest. Initial preview testing correlated browser
use with TCP writes as long as 1.28 seconds and capture falling from 31-34 FPS
to 17-24 FPS. TCP ingestion now runs in a separate process from HTTP delivery,
using a bounded, nonblocking latest-frame handoff and no disk recording.

Purpose: display the stream without changing the board transport.

Implementation:

- Add an HTTP endpoint on the PC that serves the latest in-memory images as an
  MJPEG multipart response.
- Keep TCP ingestion and browser delivery separate.
- Do not poll `/latest.jpg` on a timer.
- Keep disk recording disabled.

Acceptance:

- Browser frame changes track the receiver FPS.
- Opening or closing the browser does not change board upload timing.

## Stage 8 - Dual-Core Separation

Purpose: isolate capture from encode/send only after the sequential pipeline is
known to work.

Implementation:

- Pin camera dequeue/requeue to HP core 0.
- Pin JPEG encode and TCP write to HP core 1.
- Add the smallest possible latest-frame handoff.
- Add one buffer and one handoff mechanism only; do not add a generalized
  pipeline manager.
- Keep the Stage 6 log fields so before/after timing can be compared.

Acceptance:

- Capture timing is no worse than the sequential Stage 6 result.
- Stream FPS is unchanged or improved.
- No stale backlog accumulates.

## Stage 9 - Reliability Features

Add each item as a separate board-tested change:

1. TCP reconnect after disconnect.
2. Bounded socket-write timeout.
3. Dropped-frame counters.
4. Optional CRC32 validation.
5. Optional disk recording on the PC outside the receive path.
6. Runtime JPEG quality control.
7. Runtime resolution control.

Do not add the next guard until the previous change has passed a sustained
hardware test.

## Test Record Template

Record this information for every burn:

```text
Stage:
Firmware revision/identifier:
Arduino ESP32 core version:
Build options:
JPEG FPS and quality:
Camera FPS:
Encode time:
Send time:
Receiver FPS:
Observed errors:
Result: PASS / FAIL
```

## Definition of Completion

The bring-up is complete when the board continuously captures OV5647 frames,
encodes selected frames as JPEG, sends them over one persistent connection, and
the PC displays the live JPEG stream at the verified target rate. Production
guards and remote controls remain outside this bring-up scope.
