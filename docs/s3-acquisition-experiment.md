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

## Wi-Fi / SD EIO investigation V1

The RAW/SD pipeline parameters remained unchanged throughout this investigation:
83,333 samples/s, 4,096 `uint16_t` samples and 8,192 bytes per chunk, 12-chunk
pool, existing queues, checkpoints, CRC, and R1/R3 recovery. Unless explicitly
stated, network tests used STA only, normal power save and TX power, with no AP,
HTTP, mDNS, OTA, or intentional traffic.

| Test | Configuration | Duration | First EIO | R1 / RAW failures | Terminal result | Loss / pool | Queue free/pending/ready | Write avg/max us | Checkpoint avg/max us | Writer hold max us | Internal/DMA min bytes |
|---|---|---:|---:|---:|---|---:|---|---|---|---:|---:|
| N0 | Network disabled | 59.900 s | - | 2 / 2 | none; PASS | 0 / 0 | 5 / 7 / 6 | 19,476 / 104,766 | 81,560 / 134,863 | 177,975 | not instrumented |
| T1 | `WIFI_STA`, no `WiFi.begin()` | 59.912 s | - | 0 / 0 | none; PASS | 0 / 0 | 2 / 10 / 9 | 19,172 / 107,146 | 71,311 / 94,638 | 202,908 | 66,960 / 59,200 |
| T2 | One unsuccessful association attempt, no application reconnect loop | 59.857 s | - | 0 / 0 | none; PASS | 0 / 0 | 8 / 4 / 3 | 18,763 / 58,829 | 69,287 / 135,912 | 156,041 | 67,616 / 59,856 |
| T3 | STA associated | 4.989 s | not instrumented | 4 / 5 | `raw_retry`; FAIL | 4,096 / 0 | 8 / 4 / 3 | 19,165 / 41,719 | 149,824 / 149,824 | 169,239 | 61,636 / 53,876 |
| T4 | STA associated, power save disabled | 11.748 s | not instrumented | 11 / 12 | `raw_retry`; FAIL | 4,096 / 0 | 4 / 8 / 7 | 21,573 / 120,001 | 110,193 / 134,205 | 153,883 | 61,596 / 53,836 |
| T5 | STA associated, no intentional traffic | 55.549 s | 1.698 s | 27 / 27 | secondary pool exhaustion after 1.180 s checkpoint; FAIL | 4,096 / 1 | 0 / 12 / 11 | 19,527 / 119,899 | 132,205 / 1,179,526 | 1,233,149 | 61,616 / 53,856 |
| T6 | STA associated, one ICMP ping/s | 4.944 s | 0.267 s | 7 / 8 | `raw_retry`; FAIL | 4,096 / 0 | 7 / 5 / 4 | 21,268 / 120,718 | 138,290 / 138,290 | 157,787 | 61,472 / 53,712 |
| T7 | STA associated, TX reduced from 20 dBm to 5 dBm | 3.639 s | 3.377 s | 1 / 2 | `raw_retry`; FAIL | 4,096 / 0 | 8 / 4 / 3 | 18,709 / 24,033 | 135,145 / 135,145 | 155,115 | 61,604 / 53,844 |
| T8 | STA associated, writer priority 2 -> 3, writer still core 0 | 18.068 s | 1.748 s | 10 / 10 | `map`; FAIL | 0 / 0 | 9 / 3 / 2 | 19,456 / 120,875 | 66,293 / 102,676 | 122,802 | 61,604 / 53,844 |
| T9 | STA associated, writer priority 2, writer moved core 0 -> 1 | 10.648 s | 2.191 s | 9 / 10 | `raw_retry`; FAIL | 4,096 / 0 | 9 / 3 / 2 | 19,015 / 32,902 | 76,841 / 102,914 | 122,602 | 61,608 / 53,848 |

The original full-network test also failed after 11.992 s with 21 R1 recoveries,
one retry failure, one SD error, and 4,096 lost samples. The later isolated tests
show that AP, HTTP, mDNS, OTA, polling, and application traffic are not required
to reproduce the issue.

### Demonstrated conclusions

- The driver and active STA mode are stable for 60 seconds without an effective
  association; repeatable RAW write `EIO` failures begin once STA is associated.
- Disabling Wi-Fi power save, reducing configured TX power to 5 dBm, increasing
  writer priority, and moving the writer away from the ESP-IDF Wi-Fi core do not
  remove the failures in these single tests.
- Terminal errors were identified as failed RAW retries (`raw_retry`), a map
  write failure (`map`), or a secondary pool exhaustion following a long
  checkpoint. Initial RAW failures commonly recovered as R1 before termination.
- STA remained associated with RSSI approximately -47 to -56 dBm and no observed
  reconnects. Internal and DMA-capable memory remained available and comparable
  at failure. Most failures occurred without pool exhaustion.
- Result dispersion is large. A longer time to failure in one run is not evidence
  of a correction, and the tests do not establish a physical root cause.

Open hypotheses include SD-card or SPI electrical sensitivity during associated
radio operation, supply transients local to the Sense microSD interface, and an
interaction inside the Wi-Fi/SD/SPI software stack not resolved by simple task
priority or core affinity changes. The next planned experiment is a strict A/B
with a second physical microSD using the T5 reference configuration.

## Physical microSD A/B/A/C campaign

All tests below used the same flashed firmware and T5 reference configuration:
producer core 1 / priority 4, writer core 0 / priority 2, associated STA with
normal power save and TX power, and no AP, HTTP, mDNS, OTA, or intentional
network traffic. No card was reformatted by the experiment.

| Physical card / test | Duration | RAW bytes | First EIO | RAW failures / R1 | Retry / reopen failures | Terminal SD error | Loss / pool | Invariant | Result |
|---|---:|---:|---:|---:|---:|---|---:|---|---|
| Original failing card, T12 return-A | 37.276135 s | 5,947,392 | 2.191779 s | 21 / 19 | 2 / 0 | 2 x `raw_retry` | 12,288 / 1 | false | FAIL |
| Raspberry Pi 16 GB card, T11 | 59.333278 s | 9,822,208 | none | 0 / 0 | 0 / 0 | none | 0 / 0 | true | PASS |
| Raspberry Pi 16 GB card, T11b | 59.949187 s | 9,920,512 | none | 0 / 0 | 0 / 0 | none | 0 / 0 | true | PASS |
| Raspberry Pi 16 GB card, long qualification | 600.031823 s | 99,581,952 | 91.460971 s | 4 / 4 | 0 / 0 | none | 0 / 0 | true | PASS |
| Kingston 16 GB `SDC10G2/16GB`, T13 | 59.616895 s | 9,879,552 | 18.238215 s | 4 / 4 | 0 / 0 | none | 0 / 0 | true | PASS |
| Kingston 16 GB `SDC10G2/16GB`, T13b | 59.786869 s | 9,887,744 | none | 0 / 0 | 0 / 0 | none | 0 / 0 | true | PASS |

### Conclusions bounded by the measurements

- The A/B/A/C sequence demonstrates a strong dependence on the physical microSD:
  the original card repeatedly produces frequent EIO failures and eventually
  exceeds the available recovery, while two independent cards complete the
  tested acquisitions without data loss.
- A passing card is not necessarily EIO-free. The Raspberry Pi card had four
  recovered incidents during 600 seconds, and the Kingston had four during T13.
- R1 reopen/reposition/retry recovery preserved the data invariant when those
  occasional incidents occurred. It must remain present and instrumented.
- A successful `SD.begin()` and SDHC detection are insufficient qualification
  for sustained RAW acquisition. Qualification must exercise sustained writes
  and verify recovery, loss, CRC/invariant, and finalization.
- These results do not establish Wi-Fi as the physical cause and do not identify
  the internal or electrical mechanism responsible for EIO.

## SDINFO diagnostic scope

The serial `SDINFO` command is available only outside acquisition and uses the
public Arduino-ESP32 `SD` API. With the current SD-over-SPI backend it reports
card type, raw capacity, sector count, sector size, and filesystem total/used
space. CID and CSD are not exposed cleanly, so MID, OID, PNM, PRV, PSN, and MDT
remain unavailable. No electronic identification of the actual manufacturer or
commercial product may be inferred from the available fields.
