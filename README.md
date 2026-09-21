# KNX Analyzer — Jalon 3

Firmware for Waveshare ESP32-C6-Touch-LCD-1.47, Arduino ESP32 core 3.3.11. LCD, touch, SD presence, QMI8658A and GPIO5 ADC have been exercised. The SD card is mounted only for a presence probe, then unmounted. It is never erased or written. TP-UART, KNX and GPIO6/VBUS are outside this milestone.

## Build and flash

```powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --output-dir build firmware/KNXAnalyzer
arduino-cli upload -p COM10 --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --input-dir build firmware/KNXAnalyzer
```

USB Serial uses 115200 baud and emits JSON records. S starts analysis and X stops it; R, F and M arm rising, falling and manual triggers. LCD and HTTP use the same analysis backend.

## Wi-Fi provisioning

At first boot, the device creates the open AP `KNX-Analyzer-XXXX`, with a stable chip-derived suffix and gateway `192.168.4.1`. The LCD shows the SSID and address. Connect to the AP, open `http://192.168.4.1/`, and use Configuration to scan and save a Primary or Backup Wi-Fi profile. Both profiles are stored in ESP32 NVS; passwords are never returned by the API or logged. The firmware tries Primary for 12 seconds and then Backup for 12 seconds. If neither connects, it creates the AP. After a station disconnects for 10 seconds, the sequence is retried. Future factory reset can clear the `knxwifi` NVS namespace.

The local Web UI and API use HTTP port 80; live status uses WebSocket port 81. Dashboard and Scope are functional. Events, Sessions and System are placeholders. Scope data is reduced on the ESP to 160 min/max columns; the full 50,000-sample ring remains in RAM. No external web assets are loaded.

## API

- `GET /api/status`, `POST /api/analysis/start`, `POST /api/analysis/stop`
- `GET /api/scope/status`, `POST /api/scope/arm` with `{"mode":"rising|falling|manual"}`, `POST /api/scope/trigger`, `POST /api/scope/clear`, `GET /api/scope/capture`
- `GET /api/wifi/status`, `GET /api/wifi/networks?refresh=1`, `POST /api/wifi/profiles` with `{"slot":1|2,"ssid":"...","password":"..."}`

## ADC

GPIO5 is sampled by the native ESP-IDF continuous ADC/DMA driver at a requested 83,333 samples/s, the installed core's upper limit for ESP32-C6. A dedicated task drains 1024-byte frames into a 50,000 × 16-bit ring (100 KB); the driver has a 32 KB pool and an overflow callback. Captures keep 35,000 samples before the trigger and 15,000 including and after it. Rising and falling physical triggers were validated in Jalon 2 at about 83.3 kS/s with zero overruns. The trigger threshold is 2048 raw counts.

`tools/adc_load_test.ps1` runs a 10-minute Serial acquisition check on COM10. Web and interactive tests require a reachable AP or station address.
