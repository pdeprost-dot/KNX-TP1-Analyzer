# KNX TP1 Analyzer — Current Checkpoint

## Acquisition

- Field Acquisition V1 is frozen and considered stable.
- Commit: `00ffc40ccd80916c4a56d09f437b84b319d4d0e2`.
- Tag: `field-analyzer-v1`.

## Network

- Network Session API V1: READY.
- HTTP Range, resume by offset, and persistent-cache foundations are validated.

## Studio

- Studio Network Import V1: READY.
- LOCAL/SD import remains supported.
- Network event RAW is fetched on demand and CRC-validated.
- Offline and network decoding are identical.

## Decoder

- Historical thresholds: `1600/1700`.
- FieldCandidate thresholds: `1715/1815`.
- Validated field corpus: 987/987 parity OK.
- This result is not generalized to every KNX network.

## Current direction

Priority: `Studio Network Import and data analysis`.

Short-term goal: make Studio usable enough to capture and analyze an approximately one-hour session on the home network.

## Later

Only after this phase:

- BUS CHECK DC;
- TP1 auto-calibration;
- school-network testing;
- multi-Analyzer and cross-analyzer synchronization/comparison;
- optional SD/Wi-Fi optimization;
- power-loss hardening if genuinely needed.
