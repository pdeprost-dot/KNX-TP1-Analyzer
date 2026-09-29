# KNX TP1 Analyzer — Monorepo Checkpoint

## Repository layout

- **pdeprost-dot/KNX-TP1-Analyzer** is the official canonical repository for
  all Firmware, Core, Studio, test, and shared-documentation development.
- **pdeprost-dot/KNX-TP1-Analyzer-Studio** is retained only as a historical
  repository. Do not create new or parallel Studio development there.
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

## Studio Event RAW Viewer V1

- Studio reconstructs an event lazily from the `knx-long-session-1.0`
  `uint16_le` RAW chunks, preserving 64-bit sample positions and validating
  every chunk CRC before display.
- The firmware D44 definition is reproduced deterministically in Core, using
  available pre-event sample context; ADC and D44 graphs share navigation and
  expose the recorded session threshold.
- Missing chunks, gaps, incomplete data and invalid CRC are explicit states;
  no known gap is drawn as a continuous signal.
- Real session **KNX-F35F14A4** validated: **18/18** events reconstructed and
  all referenced chunks CRC-valid. Human Windows validation covered ADC/D44
  rendering, threshold markers, cursor, zoom, pan, fit, previous/next and the
  vertically scrollable layout.
- Release validation: Core **21/21** and Desktop **5/5** tests passing. This
  milestone adds no new TP1 decoding and keeps the existing decoder profiles
  unchanged.

## Studio deterministic TP1 analysis — current development state

- Core contains the recovered C6-derived deterministic pipeline from analog
  pulses through bit slots, characters and bounded records, including parity,
  timing, checksum, ACK/BUSY and incomplete/analog-undecoded diagnostics.
- The RAW Viewer exposes synchronized pulse, slot/bit, character, record,
  error and ACK overlays with interactive record navigation.
- Historical **1600/1700** and FieldCandidate **1715/1815** remain explicit,
  unchanged profiles. FieldCandidate has useful results on session
  **KNX-F35F14A4**, but the decoder and thresholds are not claimed to be
  universal or fully validated for every TP1 installation.

## Continuous RAW V1 — XIAO qualification

- Firmware schema **knx-long-session-1.1** adds `CONTINUOUS_RAW` while keeping
  EVENT compatibility. Samples are authoritative `uint16_le`, in chunks of
  **4096 samples / 8192 bytes** with sample intervals and per-chunk CRC32.
- During Continuous RAW capture Wi-Fi is completely OFF and D44, TP1 decoding,
  event selection and calibration filtering are not applied. The path is ADC
  to DMA, fixed buffers and segmented SD storage only.
- `session_duration_us` measures START-to-CLOSED wall time;
  `adc_capture_duration_us` measures the actual ADC production window, and the
  measured rate uses stored samples over that ADC duration.
- The physical SD `manifest.json` is authoritative and streamed directly by
  the Session API through the fixed-buffer, partial-write-safe file transport.
  Studio imports metadata without loading the complete RAW and validates only
  requested ranges/chunks through CRC.
- Qualified sessions on the current XIAO ESP32-S3 and installed SD card:
  - **KNX-9E1255F7**, ADC 9.979996 s, 831488 samples, 83315.464255 Hz,
    1662976 bytes, 203 chunks;
  - **KNX-AD3FC48E**, ADC 59.979802 s, 4998144 samples, 83330.451808 Hz,
    9996288 bytes, 1221 chunks;
  - **KNX-ECA32742**, ADC 299.976798 s, 24997888 samples, 83332.738287 Hz,
    49995776 bytes, 6103 chunks.
- All three have zero loss/gaps and pass Session API plus distributed Studio
  CRC validation. The 300 s session also has zero DMA, ADC, SD and pool errors,
  true heap/storage invariants and CLOSED/COMPLETE finalization.
- Qualification conclusion: **Continuous RAW V1 is qualified on the tested
  XIAO ESP32-S3 plus current SD for 300 s, from ADC through Studio.** It is not
  generalized to other Analyzer hardware.
- Known limits: ESP-IDF Wi-Fi deinitialization messages remain; the 60 s run
  observed transient minima of 2464 internal bytes, 1876 DMA bytes and
  884-byte largest blocks without corruption or failure.

## Field Campaign V1

- Firmware V0.8 adds only a preparation/metadata layer around the qualified
  Continuous RAW pipeline. ADC, DMA, fixed buffers, 4096-sample chunks, SD
  writer, integrity and finalization are unchanged; Wi-Fi remains OFF during
  capture and no D44/TP1 analysis runs in this mode.
- The Web form offers 10, 60 and 300 second presets plus 10–32400 seconds
  custom duration. `site`, `bus` and `point` are limited to 64 UTF-8 bytes and
  `note` to 160 bytes. Empty values are valid and TP1 calibration is informative,
  not blocking, for Continuous RAW.
- START validates IDLE/CLOSED state, HEALTHY storage, SD presence, mode,
  duration, text limits and estimated RAW space plus a 16 MiB margin before
  scheduling acquisition. Values are frozen in the versioned
  `field_campaign` section of `session-start.json` and authoritative
  `manifest.json`.
- Studio displays the optional campaign metadata while retaining compatibility
  with earlier EVENT, V0.7 Continuous RAW and transitional flat-label sessions.
- Real validation session **KNX-135D1BFC**: 831488 samples, 203 chunks,
  1662976 bytes, zero loss/gaps and zero DMA/ADC/SD/pool errors, invariant true,
  CLOSED/COMPLETE. Session API and Studio bounded RAW/CRC import passed.
- The first live import ended one HTTP metadata response prematurely; one
  unchanged retry passed, with valid manifest, metadata, RAW and CRC and no
  demonstrated SD/RAW corruption. No further network investigation belongs to
  this milestone.
- The flashed image used for the 10 s validation predates only the final HTML
  escaping of user-entered campaign text. The final versioned source/build
  includes this presentation-only hardening and was not redeployed for it.

## Next gate

Do not start another acquisition or a new milestone automatically.
