# Architecture and pinned firmware format

Reference: `pdeprost-dot/KNX-TP1-Analyzer`, tag `tp1-v1`, commit `66e51a16274bc4cebe6ff055752467c873bbabad`; firmware files `storage.cpp`, `storage.h`, `tp1_decoder.cpp`, and `docs/sessions-sd-v1.md`.

The SD layout is `/knx-analyzer/sessions/<session-id>/`. `session.json` stores state (`RUNNING`, `CLOSED`, `INTERRUPTED`), uptime and nullable date/duration, event and error counters, firmware/hardware, and `capture_format`. `tp1-candidates.jsonl` contains one `TP1_CANDIDATE` per line: `monotonic_us`, nullable `date_time`, `classification`, `raw_hex`, byte count, parity/timing errors and overflow; recognized standard frames also carry source, destination, destination type, hop count and TP length. `events.jsonl` describes analog captures separately. Finalized `captures/event-000001.bin` files are available; `.part` files are ignored.

RAW version 1 has a 48-byte little-endian header, `KNXADC1\0` magic, version, event ID, sample rate, sample count, trigger index, pre/post counts, ADC min/max and CRC-32 IEEE over only the following little-endian `uint16` sample bytes. The reader checks magic, version, length and CRC. It never alters source files.

The firmware classifies one-byte `CC`, `0C`, `C0` link responses and standard frames. `VALID_UNKNOWN` means the checksum/physical checks passed while higher semantics remain uninterpreted. Core retains firmware classifications and original JSON, skips malformed lines with diagnostics, and warns about future capture versions. The UI does not reclassify candidates or infer DPTs. This leaves room for KNX/Synco LTE interpretation later.

## Historical local sessions

The 53 locally supplied sessions span development stages. All 53 session.json files report the same firmware string (0.5.0-sessions), which does not encode a Git commit. Studio therefore labels the generation from the files present (TP1 journal, empty TP1 journal, analog Events, or metadata only) and does not claim that this proves the exact tp1-v1 build. For valid standard VALID_UNKNOWN frames in older journals whose generic address fields are absent, Studio computes source, destination, address type, hop count and TP length only when the RAW length and XOR checksum are consistent. The detail panel marks these values as derived from RAW; the original JSON remains visible.

## Analog navigation and future calibration

The waveform keeps all original uint16 RAW samples. Wheel zoom changes the horizontal viewport, left-button drag pans it, Ctrl+wheel adjusts the vertical scale, and double-click or Reset/Fit restores the full capture. The trigger remains at t=0 when visible. The visible time range is calculated from the recorded sample rate and trigger index.

Historical sessions contain no ADC calibration, so Studio reports ADC calibration unavailable and shows RAW counts measured at GPIO5 after the analog front end. No RAW/4095 voltage shortcut is used. Analog captures are not assumed to correspond to KNX telegrams.

A proposed optional future session.json extension is adc_calibration with a nonempty source and ascending points of raw and millivolts values. An optional analog_front_end object can carry gpio5_per_bus_ratio and a nonempty source. Core accepts only finite, ordered calibration points, interpolates within their characterized range, and never extrapolates. Only then may Studio estimate GPIO5 mV. A valid divider ratio between zero and one additionally permits an explicitly labeled KNX bus voltage estimate. This schema is a Studio preparation, not a claim that the current firmware records these fields.
