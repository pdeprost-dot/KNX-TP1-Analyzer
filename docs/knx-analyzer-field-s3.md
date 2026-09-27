# KNX Analyzer Field S3

## Legacy C6 mapping

| C6 mechanism | Role | Hardware-dependent | S3 reuse | Required adaptation |
|---|---|---:|---|---|
| `adc_continuous` + DMA frames | Continuous sample source | Yes | Design and error counters | ADC1 channel 0 / GPIO1, measure effective rate and DMA behaviour |
| 12 × 4096-sample internal pool | Bounded pre-trigger/history and writer handoff | Partly | Yes | Requalify internal/DMA heap and task placement |
| D44 (difference of adjacent 4-sample sums) | Low-cost activity trigger | No algorithmically | Yes | Inject configuration; never reuse C6 RAW threshold blindly |
| 8333-sample pre/post windows | Preserve signal around activity | Timing, not board | Yes | Express from measured sample rate; requalify duration |
| Retrigger extends current event | Merge nearby activity and bound event metadata | No | Yes | Preserve single active event semantics |
| Selected history chunks only | Event-driven RAW volume reduction | No | Yes | Preserve; continuous RAW remains an explicit diagnostic mode |
| LS1 identity/session start | Stable Analyzer/session identity | No | Yes | Board identity source changes to S3 |
| LS2 streamed `events.jsonl` | 64-bit event metadata without RAM growth | No | Yes | Keep stable English machine fields |
| LS3 segmented RAW/map/index/CRC | Long-session bounded files and Range access | No | Yes | Reuse S3-qualified SD backend and parameters |
| LS5 checkpoints | Progressive durable state | No | Yes | Reuse S3-qualified cadence; retain measured latency metrics |
| R1 and Resilience V2 | Recover transient write failures; explicit GAP/outage/recovery | No | Yes | Reuse validated S3 implementation and completion statuses |
| Acquisition/session invariants | Account stored + ignored + lost samples | No | Yes | Event mode must include ignored samples; continuous mode has zero ignored |
| Field Web/network/OTA | Idle control and maintenance | Partly | Behaviour only | S3 Wi-Fi; services stopped and radio confirmed OFF before ADC starts |

The legacy C6 is a software source only. C6 pins, ADC threshold `500`, raw levels,
DMA sizes, task placement, SD clock and UI assumptions are not S3 defaults.

## S3 architecture and modes

`board_xiao_esp32s3.h` owns the first board pin map. Acquisition produces real
ADC samples. `activity_detector.h` owns the portable D44 primitive and injected
configuration. The sketch owns event-window selection, storage/session,
Resilience V2 and diagnostics. Network services operate only in `IDLE`/`CLOSED`.

- `EVENT`: product direction; only chunks selected by pre/post-trigger windows are stored.
- `CONTINUOUS`: explicit ADC/SD qualification mode; every chunk is stored.

Calibration is deliberately absent from this milestone. A future calibration
component must determine idle level, noise/range, activity threshold and suitable
pre/post durations on the installed analog front-end. Until then the S3 trigger
profile is explicitly `UNCALIBRATED_SAFE`; no C6 RAW threshold is claimed valid.

GPIO43/D6 and GPIO44/D7 are reserved for a future UART front-end and are not
initialized. Camera, microphone, display and touch are not initialized.

## Validated milestone: S3 analog + Web V0.4

### Current hardware

- Seeed Studio XIAO ESP32-S3.
- ADC input: D0 / GPIO1.
- A 27 kOhm resistor is installed between D0/GPIO1 and GND.
- No KNX bus is connected yet; the future 390 kOhm bus-side resistor is not connected.
- The ESP32-C6 implementation is LEGACY reference material only and is no longer a product target.

### Acquisition and storage

- ESP32-S3 ADC continuous/DMA acquisition at approximately 83.3 kSamples/s.
- The 4096-byte ADC/DMA buffer is allocated outside the task stack in deterministic internal DMA-capable memory.
- ADC task stack: 4096 bytes; validated minimum free high-water mark retained at approximately 3152 bytes.
- The relevant legacy Field detector, pre-trigger/post-trigger windows and event-driven capture semantics are retained.
- Normal Field operation stores event RAW only; continuous RAW remains a diagnostic qualification mode.
- Resilience V2 is integrated: explicit gaps, segmented storage, recovery/backoff and completion invariants.

The 0 V analog qualification ran for 600 seconds and processed approximately 50 million samples. Observed ADC min/max/mean were `0 / 56 / ~0.029`. It completed with zero event or excursion, zero EIO/R1/R3, zero loss, gap or pool exhaustion, storage `HEALTHY`, invariant true, and final state `CLOSED / COMPLETE`.

### Network and Web

- Two persistent STA profiles: Field and Office.
- Autonomous `KNX-Analyzer-XXXX` AP with a manually validated captive portal.
- Manual Wi-Fi scan and responsive smartphone/PC UI.
- Current navigation: Dashboard, Network, Update.
- Sessions is deliberately absent until a real SD-wide session index exists.
- START returns a browser-local deadline/countdown with no polling.
- Wi-Fi is absolutely OFF during acquisition: no STA, AP, DNS, HTTP, mDNS or OTA.
- STA, AP and Web return only after storage finalization.

### OTA validation

Two update paths are validated:

1. Authenticated developer ArduinoOTA directly to the known IP address.
2. User Web OTA by uploading an ESP32 application `.bin` from the Update page.

The final manual Web OTA installed `KNXAnalyzerField-s3-analog-v0.4-web-ota-test.bin`. The image size was `1,086,432` bytes and its SHA-256 was `F569E66DFED430C29804E11E5B30632390B0A245BB9391D7C4373C17B4B6E5C6`. After reboot, the Dashboard confirmed active version `KNXAnalyzerField-s3-analog-v0.4-web-ota-test`; the STA/AP configuration was preserved.

### Open items

- mDNS discovery.
- Complete SD session history/index.
- Touchscreen UI and hardware qualification for the future Waveshare ESP32-S3.
- Real KNX analog connection and analog auto-calibration.
- Later TP-UART/NCN interface.
- Potential Web OTA security hardening.

## Time reference V0.4.1

Session chronology separates the always-valid local monotonic clock from an optional absolute UTC anchor. Sample, event, segment and GAP offsets remain monotonic and do not depend on UTC availability or changes.

`session-start.json` keeps schema `knx-long-session-1.0` and adds these optional, additive fields:

- `time_schema_version`: `"1.0"`
- `time_source`: `"NTP"`, `"BROWSER"`, or `"NONE"`
- `absolute_time_valid`: JSON boolean
- `start_unix_ms`: JSON integer when valid, otherwise `null`
- `start_utc`: ISO-8601 UTC with milliseconds when valid, otherwise `null`
- `monotonic_origin_us`: decimal uint64 string from `esp_timer_get_time()`
- `browser_timezone_offset_min`: signed JSON integer for BROWSER, otherwise `null`

Web START sends `Date.now()` and the browser timezone offset automatically. Firmware relates that request-time UTC value to the actual acquisition monotonic origin. A local/serial START records `NONE`, false validity and null absolute values; it never stores a placeholder date. NTP is represented in the source model and has higher intended priority, but no NTP synchronization was added or validated in this focused milestone.

Sessions written before V0.4.1 remain compatible because all new fields are additive. Their missing time metadata must be interpreted as `absolute_time_valid=false` and `time_source=NONE`.

Short validation results:

- BROWSER: 60.058 s, `CLOSED/COMPLETE`, valid Unix/ISO UTC anchor and monotonic origin, invariant true.
- NONE: 60.108 s, `CLOSED/COMPLETE`, `start_unix_ms=null`, `start_utc=null`, invariant true.
- Reboot: SD remained `HEALTHY`, STA/AP profiles persisted, Dashboard HTTP 200, active version `KNXAnalyzerField-s3-analog-v0.4.1-time`.

Studio should derive a presentation-only session name from metadata without renaming SD folders or technical identifiers:

- valid absolute time: `DD-MM-YYYY HH:MM:SS — <Analyzer ID> — <duration>`
- invalid absolute time: `UNSYNCED — <Analyzer ID> — <duration>`

Example: `27-09-2026 17:25:17 — KNX-Analyzer-A994 — 1 min`. The displayed local date is presentation only; canonical metadata remains UTC and monotonic offsets remain the interval reference.
