# Phase 5 Arduino Test

The `v3.0-phase5-arduino` sketch adds an immutable, reference-counted
`PhotoBlob` and mutex-protected `PhotoStore`. The boot gate starts five reader
tasks while publishing 100 replacements through the JPEG allocator. Readers
hold a reference while inspecting the body and metadata, so replacement cannot
free an in-use JPEG.

The serial monitor must show:

```text
implementation version=v3.0-phase5-arduino
photo store validation status=passed replacements=100 readers=5 latest_id=101 psram_before=... psram_after=... psram_largest_before=... psram_largest_after=...
RTSP/RTP-JPEG stream ready: rtsp://<board-ip>:554/
```

Phase 5 does not expose the high-resolution photo over RTSP. The stream remains
the restored 800x800 quality-50 baseline. Run the existing five-minute RTSP
receiver and 3000-frame RTP packet probe after the boot gates pass.
