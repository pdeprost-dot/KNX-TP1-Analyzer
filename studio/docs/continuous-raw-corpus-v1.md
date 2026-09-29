# Continuous RAW Corpus V1

## Objective

RAW Corpus is an offline laboratory catalogue for completed `CONTINUOUS_RAW` acquisitions. It identifies, validates, preserves and locates captures without running the TP1 decoder or contacting an Analyzer.

## Architecture

- `RawCorpusImporter` recognizes and validates one session directory using bounded streaming reads.
- `RawCorpusCatalog` persists entries in a local JSON catalogue.
- `RawCorpusEntry` keeps acquisition facts, integrity, provenance, file inventory and separate local annotations.
- The Desktop `RAW Corpus` window provides import, filtering, details and an `Open / Analyze` action.

The default catalogue is `%LOCALAPPDATA%/KNXAnalyzerStudio/raw-corpus/catalog.json`. It contains metadata only, never RAW payloads.

## Input format and validation

An import accepts a session directory or its `manifest.json`. The acquisition must declare `CONTINUOUS_RAW`. V1 reads `session-start.json`, `manifest.json`, `test-result.json`, `chunks.jsonl` and the referenced `raw-NNNN.bin` or `raw.bin` files.

Validation covers:

- readable object manifests and supported acquisition mode;
- lifecycle/completion state;
- declared versus computed chunks, samples and RAW bytes;
- chunk continuity, byte ranges and `uint16_le` sample sizes;
- presence of RAW segments and CRC-32 of every chunk;
- logical RAW SHA-256 when declared by the session;
- reported gaps, dropped samples, ADC errors and SD errors.

The resulting status is `VALID`, `VALID_WITH_WARNINGS`, `INCOMPLETE`, `CORRUPTED` or `UNSUPPORTED`. Missing required evidence is never reported as valid.

## Content fingerprints and duplicates

Two streaming SHA-256 values are retained:

1. `LogicalRawSha256` hashes the logical RAW bytes in chunk/sample order.
2. `ContentSha256` identifies the corpus acquisition. Its input is the domain marker `KNX-RAW-CORPUS-V1\0`, the authoritative `manifest.json` bytes (or, for an incomplete legacy session without it, the start/result/chunk metadata), followed by each chunk descriptor and its logical RAW bytes.

The descriptor contains sample start/count, segment index, byte count and CRC. Paths and import dates are excluded. An existing `ContentSha256` returns `Already imported` and does not create a duplicate.

## RAW storage

V1 supports two explicit modes:

- **Managed copy (recommended):** after successful inspection, Studio copies the intact session directory into the catalogue's `entries` area.
- **Reference:** the catalogue points to the selected original directory.

The UI defaults to managed copy, but copying only occurs after the user chooses Import. Studio never rewrites, resamples, normalizes or silently relocates source RAW.

## Metadata

`field_campaign` values (site, bus, measurement point and field note) are retained as acquisition metadata. Firmware provenance uses the V0.8 manifest names `firmware`, `board_profile` and `frontend_profile`; historical aliases remain accepted. Missing values remain absent. Optional alias, laboratory comment and tags live only in the local catalogue; saving them does not change the session manifest.

## UI

The `RAW Corpus` window lists session, date, site, bus, point, duration, measured rate, samples, RAW size and integrity. Filters cover free text, site, bus, point and integrity. The detail panel shows provenance, files, validation messages and both hashes. `Open / Analyze` opens the authoritative local session location in V1; it does not introduce another analysis engine.

## Performance and limitations

RAW and metadata hashes, CRC and copies are streamed. Validation uses at most a 64 KiB RAW buffer per chunk and does not load a capture into memory. JSON catalogue persistence is intended for tens to hundreds of entries.

Automated coverage uses generated representative fixtures, including a 256-chunk/2 MiB streaming case. Real validation used session `KNX-EF251AD1`: 4,994,048 samples, 9,988,096 RAW bytes, 1,220 chunks, all chunk CRCs valid and logical RAW SHA-256 `7A47C00024744004DC0002A33CDAA59495B6B9AD4FCB5D52F9EDD07CFDE0B3BD`. Managed-copy import, duplicate detection and catalogue reload passed. V1 does not analyze TP1 or synchronize with cloud services.

## Field acquisition workflow

1. Retrieve the complete session directory after the field campaign.
2. Preserve that directory and its RAW files unchanged.
3. In Studio, open `RAW Corpus`, choose managed copy or reference, then import the session folder.
4. Confirm the integrity status and review every warning.
5. Optionally add local alias, comment and tags.
6. Never edit the authoritative source RAW; regenerate future derived data from it.
