# KNX TP1 Analyzer

## Project

- ESP32-C6 Field Analyzer and PC Studio.
- Use Arduino IDE or Arduino CLI, never PlatformIO.

## Stable acquisition baseline

- Immutable release: `field-analyzer-v1`.
- Commit: `00ffc40ccd80916c4a56d09f437b84b319d4d0e2`.
- Never modify, move, or rewrite this tag.

## Development

- Normal C6 development uses LAN and OTA. USB/COM10 is reserved for exceptional recovery.
- Perform OTA only while the analyzer is `IDLE` or `CLOSED`, never while `CAPTURING` or `FINALIZING`.
- Do not modify firmware when work concerns Studio only.
- Prefer short, targeted tests.

## Language

- User-facing UI and documentation: French and English.
- APIs, JSON, filenames, and machine formats: stable English.
- Data formats must not depend on the UI language.

## Field format and network

- Field format: `knx-long-session-1.0`.
- RAW is segmented; sample indexes and event timestamps are 64-bit.
- Metadata is progressive, CRC is streamed, and partial access uses indexes plus HTTP Range.
- Network Session API V1 is validated.
- HTTP Range and offset resume are validated byte-perfect.
- RAW download is forbidden during `CAPTURING` and `FINALIZING`.

## Studio

- Preserve LOCAL/SD import support.
- Network Import V1 is validated.
- Fetch network event RAW on demand and cache persistently by analyzer/session.
- Validate CRC before use.
- Offline and network decoding must remain identical.

## TP1 decoder

- Historical thresholds: `1600/1700`.
- FieldCandidate thresholds: `1715/1815`.
- FieldCandidate is confirmed on the tested installation, not universal.
- Do not change these thresholds automatically without a new calibration milestone.

## Known limitations

- SSID2 fallback: NOT TESTED.
- LS5/LS5B power-loss hardening: PAUSED / NOT VALIDATED.
- Reliable power is required during `CAPTURING`.
- Future work: auto-calibration, BUS CHECK DC, and multi-Analyzer.

## Working rules

- One milestone has one measurable objective; never start the next milestone automatically.
- Stop at the first significant anomaly and avoid opportunistic refactoring.
- Do not run long benchmarks without explicit need or issue periodic waiting reports.
- Request user intervention only when necessary; act autonomously for safe, reversible work.
- Be economical with tokens. Keep final reports short: results, anomalies, Git, NEXT.
- Do not commit, push, or tag unless explicitly requested or required by the milestone prompt.
