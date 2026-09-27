# KNX Analyzer Field S3 — Session Download API V1

Outside acquisition, the read-only Session API lists SD sessions, returns a
per-session manifest and streams only whitelisted metadata or segmented RAW
files. HTTP Range is supported for event RAW; session identifiers and filenames
are validated and no delete, rename or upload route exists. IDLE ADC observation
is suspended only while SD session metadata or file bytes are being read, then
resumed. During acquisition the existing Wi-Fi-off lifecycle makes the API
unavailable.

Studio lists sessions before import, publishes downloaded metadata only after
complete size validation, and fetches only the chunks required for the selected
event. Chunk CRC32 is checked before RAW is decoded or cached. Both
`sample_trigger` and the S3 field name `trigger_sample` remain supported.

## Canonical XIAO build

The reference XIAO ESP32-S3 Sense requires its 8 MiB OPI PSRAM. With
Arduino-ESP32 3.3.11, the canonical FQBN is:

`esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,PSRAM=opi`

Build Field OTA images with:

`powershell -ExecutionPolicy Bypass -File tools/build-knx-analyzer-field.ps1`

The sketch has a compile-time guard for `BOARD_HAS_PSRAM` and an early runtime
check requiring at least 7 MiB of detected PSRAM before SD, ADC, calibration or
Wi-Fi initialization. This prevents recurrence of the validated failure mode:
without `PSRAM=opi`, only 49,444 internal bytes remained before Wi-Fi and the
driver allocated one of four required RX buffers; with the canonical profile,
105,400 internal bytes remained and STA/AP/Web initialized normally.
