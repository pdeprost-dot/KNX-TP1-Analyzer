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

## Next gate

Prepare the first real field session. Do not start auto-calibration, BUS CHECK DC, multi-Analyzer, or a new acquisition automatically.
