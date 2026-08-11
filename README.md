# ESP32P4 Video Transmission

- Board: ESP32-P4 CB V1.3
- Camera: OV5647 MIPI-CSI
- Wireless: ESP32-C6 via ESP-Hosted SDIO
- Camera capture: validated
- Wi-Fi connection: validated
- PC health request: validated

## Progress

- [x] Step 1: headless camera capture complete
- [x] Step 2: wireless connection complete
- [ ] Step 3: JPEG frame transfer to PC pending
- [ ] Step 4: runtime resolution switching pending
- [ ] Step 5: runtime JPEG quality and controls pending
- [ ] Step 6: AI image recognition validation pending

## Files

- PC receiver: `tools/pc_receiver.py`
- Implementation plan: `docs/OV5647_INCREMENTAL_JPEG_STREAM_PLAN.md`

## PC Live Preview

Start the receiver before the board:

```bash
python3 tools/pc_receiver.py --port 5001 --http-port 8080
```

Open `http://localhost:8080/` in a browser. JPEG frames remain in memory and are
not written to disk.
