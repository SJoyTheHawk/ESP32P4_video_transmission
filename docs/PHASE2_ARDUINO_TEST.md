# Phase 2 Arduino Test

## Scope

Firmware `v3.0-phase2-arduino` adds:

- a 5000 ms ESP-Video dequeue timeout configured after the capture device is
  opened;
- a deterministic timeout test that holds every buffer available from the
  driver, waits for the next dequeue to time out, releases the buffers, and
  captures a recovery frame;
- a JPEG output buffer with deterministic `release()` and `detach()` ownership;
- 100 JPEG allocation/release cycles and 100 encoder create/destroy cycles at
  boot;
- memory comparison before and after the lifecycle test.

The test firmware pauses for about five seconds during the intentional dequeue
timeout. This is expected.

## Arduino Core Compatibility

Arduino-ESP32 3.3.11 includes `VIDIOC_S_DQBUF_TIMEOUT` and
`VIDIOC_G_DQBUF_TIMEOUT`, so no precompiled archive is patched. ESP-Video
resets the timeout to an infinite wait every time a device is opened. The
sketch therefore opens the capture device first, then configures a temporary
second reference; its close leaves the finite timeout in the shared driver
object used by `capture_dev`.

The bundled ESP-Video implementation returns `ESP_FAIL` when the bounded
receive expires; the VFS exposes that as `EPERM`. The Phase 2 test recognizes
that driver result only when the measured wait is 4.5-6.5 seconds and reports
the application-level timeout as `ETIMEDOUT`. The source-managed ESP-IDF
implementation remains responsible for changing the driver return to
`ESP_ERR_TIMEOUT` directly.

Binary inspection of the pinned JPEG driver confirms that
`jpeg_alloc_encoder_mem()` allocates through ESP-IDF's heap-capability calloc
functions. Those allocations use the standard `free()` release path used by
the adapter.

This is an explicit Arduino-format compatibility adaptation to the V3 guide,
which was written for a source-managed ESP-Video component patched to return
`ESP_ERR_TIMEOUT` directly.

## Expected Serial Results

The boot log must contain all of these outcomes:

```text
implementation version=v3.0-phase2-arduino
capture timeout status=configured requested_ms=5000 actual_us=5000000 ...
jpeg lifecycle status=passed cycles=100 capacity=2097152 alloc_release=passed engine_create_destroy=passed detach=passed ...
capture timeout test status=passed injection=hold-available-buffers requested_ms=5000 ... reported_errno=116 stream_started=yes recovery_frame=valid
```

`driver_errno=1` is expected with the pinned core (`EPERM`).
`reported_errno=116` is `ETIMEDOUT`.

The JPEG lifecycle line must also show:

- free heap after the cycles is not lower than before;
- largest heap block after the cycles is not lower than before;
- free PSRAM after the cycles is not lower than before;
- largest PSRAM block after the cycles is not lower than before.

After these boot gates, the normal 800x800 quality-50 RTSP stream must start and
remain stable for five minutes at:

```text
rtsp://192.168.1.91:554/
```

## Exit Gate

Phase 2 passes only when all three Phase 2 outcome lines report the expected
configured/passed state, the timeout duration is within 4500-6500 ms, the
recovery frame is valid, and the five-minute RTSP baseline remains stable. Any
`Phase 2 gate failed` message blocks Phase 3.
