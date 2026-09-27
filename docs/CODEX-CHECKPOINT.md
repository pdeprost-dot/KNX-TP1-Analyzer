# KNX TP1 Analyzer — Monorepo Checkpoint

## Repository layout

- The firmware remains in its existing root structure: **firmware/**, **tools/**, and root **docs/**.
- Studio is imported under **studio/**.
- Studio history was imported without squash.
- Monorepo subtree import commit: **0cea6a4182425631b93564e861730d54a1eddf62**.

## Frozen baselines

- Firmware Field Acquisition V1: tag **field-analyzer-v1**, commit **00ffc40ccd80916c4a56d09f437b84b319d4d0e2**.
- Firmware HEAD before monorepo: **c46f0fc6c2bfee5155fbbae980daa13d1ec86d1a**.
- Studio Network Import V1: commit **52c90ae18a638aa732078098956ea24e3d6b1d43**.
- Studio V0: tag **studio-v0**, commit **43ec68021e7efa4a4937ac1dc4f0cbab0acd98c6**.

## Firmware acquisition

- Field Acquisition V1 is frozen.
- Field format: **knx-long-session-1.0**.
- Network Session API V1, HTTP Range, offset resume, streamed CRC, and SD recovery R1 are validated.
- Existing firmware paths and behavior were not changed by the monorepo migration.

## Studio

- Network Import V1 is frozen.
- LOCAL/SD import remains supported.
- Network RAW is fetched on demand, cached persistently, CRC-validated, and decoded by the same offline engine.
- Historical thresholds: **1600/1700**.
- FieldCandidate thresholds: **1715/1815**; confirmed only on the tested installation.
- Known Studio validation before migration: Release build with zero warnings and 17/17 tests passing.

## Local environment

The following Studio directories remain local and ignored:

- **studio/.dotnet**;
- **studio/.firmware-reference**;
- **studio/.validation**.

No private session, RAW capture, validation bundle, or local SDK belongs in Git.

## Studio Field Analysis V1

- Frozen milestone: tag **studio-field-analysis-v1**.
- Studio presents Analyzer, session state/duration, event count, and available RAW volume.
- The event timeline keeps 64-bit positions and loads only the selected event RAW.
- LOCAL/SD segmented sessions and Network Import V1 share the same CRC-validated offline decode path and persistent network cache.
- RAW visualization supports time zoom, pan, trigger-relative navigation, and active Historical **1600/1700** or FieldCandidate **1715/1815** profiles.
- Validation baseline: .NET SDK **10.0.401**, Release build with zero warnings and errors, **19/19 tests passing**.
- Firmware and acquisition formats were not modified.

## KNX Analyzer Field S3 Auto-Calibration V1

- Development remains on feature/knx-analyzer-field-s3; the C6 firmware is
  legacy reference material only.
- The real-bus selected-chunk lifecycle fix is validated beyond the 12-chunk
  pool: 120 event chunks written, recycled and reused without exhaustion.
- Auto-Calibration V1 observes D44 in IDLE without RAW or SD writes, blocks
  START until valid, persists a versioned CRC-protected NVS snapshot and freezes
  its threshold for each session.
- Validated field calibration: threshold 1262, confidence HIGH/100%,
  noise_upper=286, activity_p10=2239, CRC 73DCD6FB.
- Calibrated 60 s acquisition: CLOSED / COMPLETE, invariant true, 18 events,
  120 RAW chunks, and zero loss, GAP, EIO, R1/R3, DMA/read or lifecycle error.
- Physical power-cycle restored the same calibration directly from NVS and
  resumed IDLE observation with STA/AP/Web and storage healthy.

## KNX Analyzer Field S3 Session API V1

- Firmware V0.6 exposes a read-only session list, manifest and whitelisted
  session files over LAN; RAW supports HTTP Range and remains unavailable
  while capturing or finalizing.
- Studio lists remote sessions, imports metadata atomically, and downloads only
  the selected event RAW through its persistent CRC-validated cache. The real
  session **KNX-F35F14A4** was validated with 18 events and 983040 RAW bytes.
- XIAO Field builds require the canonical FQBN
  `esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,PSRAM=opi`.
  A sketch-local compile guard rejects builds without OPI PSRAM, and an early
  runtime guard requires a detected capacity of at least 7 MiB before any
  acquisition, calibration or network initialization.
- The canonical Web OTA path was revalidated: PSRAM enabled, pre-Wi-Fi internal
  heap about 105 kB, STA/AP/HTTP restored, calibration NVS unchanged, and the
  Session API readable after autonomous reboot.
- Studio validation: .NET SDK **10.0.401**, Release build successful, Core
  **17/17** and Desktop **4/4** tests passing. NU1900 remains an environmental
  warning because the NuGet vulnerability feed was unavailable.

## Next gate

Do not start another acquisition or a new milestone automatically.
