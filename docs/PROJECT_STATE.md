# KNX TP1 Analyzer — Project State

## Canonical repository

Repository: `pdeprost-dot/KNX-TP1-Analyzer`
This file is the canonical short development context; detailed evidence remains
in Git and the other documentation. The historical standalone Studio repository
is not a development target.

## Product baselines

### Firmware

- Validated commit: `c87d06552bef3e39a1fe1cad83e36041930d2104`.
- Tag: `knx-analyzer-field-v0.8.1-range-fix`.
- Reference target: Seeed Studio XIAO ESP32-S3 prototype.
- Image: `KNXAnalyzerField-s3-analog-v0.8.1-range-write-fix`.
- ESP32-C6 is legacy reference material only.
- `field-analyzer-v1` / `00ffc40c` remains immutable.

Canonical XIAO FQBN:

`esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,PSRAM=opi`

### Studio

- Validated commit: `58d9609f8bab7e48b85399b5cd6b799b853e7a59`.
- Tag: `knx-analyzer-studio-integrated-analysis-v1`.
- SDK: .NET 10.0.401.
- Release build: PASS.
- Core tests: 60/60 PASS.
- Desktop tests: 7/7 PASS.
- Windows manual validation: PASS.

## Product architecture

Data path:

`Firmware ADC/DMA → RAW chunks on SD → Session API/local SD → RAW Corpus → Studio`

Firmware owns acquisition, storage integrity and authoritative metadata.
Studio and Corpus are read-only consumers. Original RAW is never rewritten.

RAW storage uses:

- `uint16_le` samples;
- 4096 samples / 8192 bytes per chunk;
- 64-bit sample positions;
- per-chunk CRC32;
- explicit continuity, loss and GAP metadata;
- the physical `manifest.json` as authoritative session manifest.

## Validated firmware functions

- ESP32-S3 ADC continuous/DMA acquisition near 83.3 kSamples/s.
- EVENT acquisition with pre/post-trigger RAW windows.
- Continuous RAW acquisition through 300 seconds.
- Resilience V2 lifecycle and explicit GAP handling.
- Chunk recycling without pool exhaustion.
- R1 recovery and R3/GAP semantics.
- Auto-Calibration V1 with CRC-protected NVS persistence.
- Wi-Fi OFF throughout acquisition.
- STA/AP/Web restoration after `CLOSED`.
- Field Campaign metadata frozen at START.
- Session API V1 and byte-complete HTTP Range transport.
- Direct-IP developer OTA and Web OTA.

Qualification applies only to the tested XIAO ESP32-S3 and SD setup.

## Validated Studio functions

- LOCAL/SD and Network Session import.
- Persistent network RAW cache and CRC validation before analysis.
- Event reconstruction from segmented RAW chunks.
- Synchronized ADC/D44 graphs, threshold and TP1 overlays.
- Event and Record navigation and All/Valid/ACK/Errors filters.
- Deterministic pulse/slot/character/record pipeline.
- KNX Telegram Parser V1.
- Explicit missing/gap/incomplete/corrupt states.
- RAW Corpus managed-copy and reference modes.
- Duplicate detection, persistent catalogue and SHA-256 identities.
- Continuous RAW Viewer using bounded range reads.
- Min/max overview, exact zoom, pan, cursor and selection statistics.

Physical decoding remains installation-dependent and is not universal.

## Reference sessions and fixtures

Event reference:

- `KNX-F35F14A4`;
- 18/18 events reconstructible;
- 983040 RAW bytes;
- chunk CRC validation PASS;
- committed replay fixture used by Core tests.

Continuous RAW qualification:

- `KNX-9E1255F7`: 10 seconds;
- `KNX-AD3FC48E`: 60 seconds;
- `KNX-ECA32742`: 300 seconds;
- zero loss/gaps in the validated campaign.

RAW Corpus references:

- `KNX-EF251AD1`: managed-copy, duplicate and persistence validation;
- `KNX-0D29B25B`: 120 seconds, 9996288 samples, 2441 chunks;
- Corpus ID `raw-ab7e9a3c99853172b6822ee0`;
- 2441/2441 CRC PASS and Integrity `VALID`;
- Continuous RAW Viewer manually validated.

Local RAW sessions and managed Corpus payloads are never committed to Git.

## Branch separation

- `feature/*` contains product work intended for consolidation.
- `research/*` contains experimental evidence and is excluded by default.
- Physical Decoder V2→V6 remains isolated at commit `71c81bd` and tag
  `knx-analyzer-physical-decoder-research-v2-v6`.
- Experimental S3 benches are evidence, not product firmware.

## Git rules

- `master` is the consolidated validated product baseline.
- Never force-push or rewrite validated tags.
- Integrate validated feature branches through an explicit consolidation PR.
- Preserve firmware and Studio histories; do not squash validated milestones.
- One milestone has one measurable objective.
- No product checkpoint, release tag or merge to master before required automated and manual validation.
- Do not commit SDKs, `.validation`, generated binaries, caches or RAW sessions.
- Firmware-only work must not modify Studio; Studio-only work must not modify firmware.
- Research remains separate until explicitly promoted.

## Active roadmap

1. Consolidate the validated firmware and Studio product baseline.
2. Build a multi-site Continuous RAW field corpus.
3. Perform comparative offline analysis across sites and front-ends.
4. Only then resume physical TP1 decoder development.

## Backlog

- RAW compression.
- AI Expert assistance.
- Final production hardware and board support.
- Touchscreen product UI.
- TP-UART/NCN integration.
- Generalized analog calibration.
- mDNS completion.
- Security hardening and other product extensions.
