# Architecture and pinned firmware format

Reference: `pdeprost-dot/KNX-TP1-Analyzer`, tag `tp1-v1`, commit `66e51a16274bc4cebe6ff055752467c873bbabad`; firmware files `storage.cpp`, `storage.h`, `tp1_decoder.cpp`, and `docs/sessions-sd-v1.md`.

The SD layout is `/knx-analyzer/sessions/<session-id>/`. `session.json` stores state (`RUNNING`, `CLOSED`, `INTERRUPTED`), uptime and nullable date/duration, event and error counters, firmware/hardware, and `capture_format`. `tp1-candidates.jsonl` contains one `TP1_CANDIDATE` per line: `monotonic_us`, nullable `date_time`, `classification`, `raw_hex`, byte count, parity/timing errors and overflow; recognized standard frames also carry source, destination, destination type, hop count and TP length. `events.jsonl` describes analog captures separately. Finalized `captures/event-000001.bin` files are available; `.part` files are ignored.

RAW version 1 has a 48-byte little-endian header, `KNXADC1\0` magic, version, event ID, sample rate, sample count, trigger index, pre/post counts, ADC min/max and CRC-32 IEEE over only the following little-endian `uint16` sample bytes. The reader checks magic, version, length and CRC. It never alters source files.

The firmware classifies one-byte `CC`, `0C`, `C0` link responses and standard frames. `VALID_UNKNOWN` means the checksum/physical checks passed while higher semantics remain uninterpreted. Core retains firmware classifications and original JSON, skips malformed lines with diagnostics, and warns about future capture versions. The UI does not reclassify candidates or infer DPTs. This leaves room for KNX/Synco LTE interpretation later.
