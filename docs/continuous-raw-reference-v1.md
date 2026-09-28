# Continuous RAW Reference Capture V1

Status: qualified on the current Seeed Studio XIAO ESP32-S3 and SD setup for
captures up to 300 seconds. This is not a universal hardware qualification.

## Session model

`knx-long-session-1.1` adds `acquisition_mode`, with `EVENT` and
`CONTINUOUS_RAW`. Both modes keep the same session directory, lifecycle,
segmented RAW, chunk map, gap journal, checkpoints, Session API and storage
resilience mechanisms. Existing `knx-long-session-1.0` sessions remain
readable and are interpreted as `EVENT` when the field is absent.

Continuous RAW stores every ADC sample delivered to the producer. D44,
calibration and event detection do not select or filter samples in this mode.
Calibration metadata is recorded for traceability but is informative only.

## RAW and integrity

- sample format: unsigned 16-bit little-endian (`uint16_le`);
- nominal rate: 83,333 samples/s; measured rate and monotonic duration are
  recorded independently;
- chunks: 4,096 samples / 8,192 bytes;
- segmented files: `raw-NNNN.bin`, described by `segments.jsonl` and
  `chunks.jsonl`;
- each chunk carries its sample interval, segment offset, byte count and IEEE
  CRC32;
- `manifest.json` records the SHA-256 of the logical stored RAW stream, meaning
  the concatenation, in sample order, of every successfully stored chunk;
- loss is never filled or hidden: `gaps.jsonl`, counters and completion status
  describe discontinuities. A session is continuous only when gap/loss counts
  are zero and the storage invariant is true.

`session-start.json`, `session-end.json`, `test-result.json` and
`manifest.json` contain the common identity, ADC, timing, calibration,
continuity, storage and completion metadata. Optional campaign fields are
`site_label`, `bus_label`, `measurement_point` and `operator_note`.

## Network and Studio

The Session API exposes the acquisition mode, final metadata, integrity
manifest, maps and segmented RAW with HTTP Range. RAW remains unavailable
during capture/finalization under the existing lifecycle rule.

For completed Continuous RAW sessions, the physical `manifest.json` on SD is
authoritative and is streamed directly through the same fixed-buffer,
partial-write-safe transport as RAW. It is not rebuilt as a complete in-memory
HTTP `String`.

Studio imports metadata first. `ContinuousRawReader` implements bounded
`ReadSamples(start, count)` semantics: it reads only overlapping chunks,
validates every chunk CRC, reports missing/incomplete/gapped data explicitly
and never loads a complete long recording by default. Network ranges reuse the
persistent analyzer/session chunk cache. The session sample start/end provide
the global timeline bounds. Event sessions keep their existing on-demand event
path unchanged.

## Web workflow

Before START, the Web UI selects EVENT or CONTINUOUS_RAW, duration and optional
campaign labels. It displays the estimated RAW size, sample format/rate, free
SD space and the calibration state. Continuous RAW does not require a valid
calibration. The validated product lifecycle remains network ON in IDLE,
complete Wi-Fi OFF during acquisition, finalization to CLOSED, then network
restoration.

## Qualification results

All three sessions completed with zero loss/gaps and passed Session API plus
bounded Studio import/CRC validation:

| Session | ADC duration | Samples | Measured rate | RAW bytes | Chunks |
| --- | ---: | ---: | ---: | ---: | ---: |
| `KNX-9E1255F7` | 9.979996 s | 831488 | 83315.464255 Hz | 1662976 | 203 |
| `KNX-AD3FC48E` | 59.979802 s | 4998144 | 83330.451808 Hz | 9996288 | 1221 |
| `KNX-ECA32742` | 299.976798 s | 24997888 | 83332.738287 Hz | 49995776 | 6103 |

The 300 s reference has zero DMA, ADC, SD and pool errors, with true heap and
storage invariants. Continuous RAW V1 is therefore qualified on this XIAO
ESP32-S3 plus the currently installed SD card, from ADC through Studio, for
300 seconds.

Known limits: Wi-Fi deinitialization can still emit ESP-IDF diagnostics, and
the 60 s run recorded transient capture-window minima of 2464 internal bytes,
1876 DMA-capable bytes and 884-byte largest blocks without corruption or
failure. These results must not be generalized to another board or SD card.
