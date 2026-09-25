# S3 Synthetic RAW/SD Experiment

Experimental bench only; this is not an ESP32-S3 product port. The XIAO
ESP32-S3 Sense camera, microphone, ADC, LCD, touch, and KNX interfaces remain
unused.

## Baseline without network

- Board: Seeed Studio XIAO ESP32-S3 Sense.
- Storage: Sense microSD over SPI, SDHC/FAT32.
- Source: deterministic `uint16_t`, 83,333 samples/s, 4,096 samples per chunk.
- Duration: 599,897,238 us.
- Chunks / samples / RAW: 12,155 / 49,786,880 / 99,573,760 bytes.
- RAW recovery: 2 R1 from 2 handle failures; no retry or reopen failure.
- Integrity: zero SD errors, loss, or pool exhaustion; invariant true; CRC32
  `69E8C876`; PASS.
- Pool: free min 3, pending max 9, ready max 8.
- RAW write latency: average 18,915.154 us; max 104,158 us.
- Checkpoint latency: average 77,181.841 us; max 237,829 us.
- Maximum writer chunk hold: 408,982 us.

This result is the comparison baseline before enabling AP/STA, HTTP, mDNS,
and authenticated ArduinoOTA around the unchanged synthetic RAW/SD pipeline.
