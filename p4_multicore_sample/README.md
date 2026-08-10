# ESP32-P4 Multicore Arduino Sample

This Arduino sketch runs one FreeRTOS worker on each ESP32-P4 high-performance
(HP) core. Each worker performs a small CPU workload and sends its result to the
Arduino `loop()` task through a FreeRTOS queue.

## Run in Arduino IDE

1. Install the latest **esp32 by Espressif Systems** package in Boards Manager.
2. Open `p4_multicore_sample.ino` in Arduino IDE.
3. Select **Tools > Board > esp32 > ESP32P4 Dev Module**, or the exact entry for
   your ESP32-P4 board.
4. Select the correct port and upload the sketch.
5. Open Serial Monitor at 115200 baud.

Expected output resembles:

```text
ESP32-P4 dual HP-core sample
Arduino setup/loop task is on HP core 1
Both HP workers started
HP0 | core=0 | iteration=1 | checksum=0x... | free stack >= ... bytes
HP1 | core=1 | iteration=1 | checksum=0x... | free stack >= ... bytes
```

The order of HP0 and HP1 reports can vary because both tasks run concurrently.

## About the LP core

The ESP32-P4 LP core is a ULP coprocessor, not a third FreeRTOS application
core. The standard Arduino-ESP32 board package does not enable or compile
LP-core programs. Using it requires an ESP-IDF project with a separately built
LP-core binary, normally through `ulp_embed_binary()`. Arduino APIs can still be
used in that project by adding Arduino as an ESP-IDF component.

Espressif references:

- [ESP32-P4 FreeRTOS overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/system/freertos.html)
- [ESP32-P4 ULP LP-core programming](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/ulp-lp-core.html)
- [Arduino as an ESP-IDF component](https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html)
