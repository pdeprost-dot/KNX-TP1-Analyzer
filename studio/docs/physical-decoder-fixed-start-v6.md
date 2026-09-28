# Physical Decoder V6 — Fixed-Start Hybrid Slot Evidence

## Scope and method

Offline-only replay of the versioned `KNX-F35F14A4` fixture. Firmware, Studio
UI, production `Tp1DeterministicDecoder`, FieldCandidate `1715/1815`, STARTs,
fractional grids, character boundaries and all 197 record boundaries are
unchanged.

V6 retains exactly 622 historical characters and 197 records. Each of their
11 slots is independently re-read from RAW with the V3 physical model:

- event-local robust baseline and median high-confidence pulse-depth reference;
- normalized relative depth and normalized median-template cosine;
- combined score `0.55 * depth + 0.45 * template`;
- `PULSE >= 0.75`, `NO_PULSE <= 0.25`, otherwise `AMBIGUOUS`;
- fixed historical expected position, ±3 samples only for physical minimum and
  template alignment;
- no parity, STOP, checksum, historical bit or KNX semantic feedback.

An initial implementation accidentally inherited V5's second ±3 slot-position
search. That violated the fixed-grid premise and allowed neighbouring slots to
influence the result. It was removed before the reported run. V6 measures each
historical slot exactly once; this is the documented adaptation from V5 back to
the V3 methodology.

## Slot comparison

`P/A/N` means `PULSE/AMBIGUOUS/NO_PULSE`.

| Slot group | Historical pulse -> V6 P/A/N | Historical no-pulse -> V6 N/A/P |
|---|---:|---:|
| START | 609 / 13 / 0 | 0 / 0 / 0 |
| DATA | 1,640 / 18 / 0 | 1,965 / 114 / 1,239 |
| PARITY | 212 / 13 / 0 | 278 / 15 / 104 |
| STOP | 46 / 2 / 0 | 485 / 30 / 59 |

V6 nearly always retains historical pulse slots, but classifies many historical
no-pulse DATA slots as pulse. This asymmetry is the main failure, not START
detection or record segmentation.

## Characters

Historical physical character baseline for this comparison:

- valid parity and STOP: 357;
- invalid parity and/or STOP: 265.

Historical valid -> V6:

- identical, strict and valid: 175 (`49.02%`);
- different, strict and valid: 77;
- ambiguous: 56 (27 byte-identical, 29 different);
- strict but invalid: 49;
- best-evidence byte identical: 202/357 (`56.58%`);
- START evidence mismatch: 7.

Historical invalid -> V6:

- strict valid: 111;
- ambiguous: 45;
- strict invalid: 109;
- valid with unchanged DATA: 2;
- valid with changed DATA: 109;
- valid with more than one changed DATA slot: 79.

Thus most apparent repairs are not independent parity-slot repairs: they rewrite
DATA, often in several positions.

## Historical INVALID_PARITY population

The 98 historical `INVALID_PARITY` records contain 241 parity-invalid
characters. V6 classifies them as:

- parity repaired with unchanged DATA: 2;
- still invalid with unchanged DATA: 7;
- physically ambiguous: 42;
- DATA changed in total: 227;
- DATA changed and strict parity valid: 124;
- DATA changed and strict parity invalid: 66.

The V6 fixed-grid signature corresponding to “historical parity pulse missing,
V6 sees pulse” occurs 62 times; 38 become complete strict-valid characters.
This is broader than V4's 43 `ANALOG_PULSE_MISSED` diagnoses because V4 also
required its independent phase criterion. Those V4 identities were not stored
as fixture metadata, so claiming a one-to-one mapping would require importing
V4's phase optimizer into V6, contrary to the frozen-grid experiment. The
important complete-character result is nevertheless clear: nearly all V6
valid outcomes alter DATA, so they cannot be called demonstrated repairs.

## Records and controls

All 197 historical boundaries are preserved exactly.

| Strict V6 classification | Count |
|---|---:|
| VALID_UNKNOWN | 6 |
| VALID_KNOWN | 53 |
| ACK byte (best evidence) | 56 |
| BUSY byte (best evidence) | 14 |
| INVALID_PARITY | 32 |
| INVALID_TIMING | 9 |
| INVALID_CHECKSUM | 2 |
| INCOMPLETE | 8 |
| ANALOG_UNDECODED | 7 |
| AMBIGUOUS | 80 |
| Telegram Parser success | 6 |

Of 62 historical ACKs, 45 remain identical strict valid, 13 are ambiguous and
6 have modified best-evidence bytes (categories may overlap for ambiguity and
byte modification). The single historical BUSY keeps its byte only as an
ambiguous record. Frozen STARTs prevent the V5 false-record explosion, but the
physical slot model still creates non-credible best-evidence control bytes.

## BC and 9C controls

- Both BC occurrences (`E8/R0`, `E9/R0`) remain exactly
  `BC FF 16 00 01 E1 00 80 CA`, strict `VALID_UNKNOWN`, parity/STOP/checksum
  valid, with minimum physical confidence `0.619` and `0.635`.
- All twelve historical 9C occurrences retain exactly
  `9C FF 16 00 01 E1 00 80 EA` as best-evidence bytes with valid
  parity/STOP/checksum.
- Three 9C occurrences are strict `VALID_UNKNOWN`; nine remain explicitly
  ambiguous with one or two ambiguous slots. No special BC/9C rule exists.

This is substantially better than free-START V5, but does not offset the loss
rate over all historically valid characters.

## Representative 11-slot paths

Notation is `slot:class:score` (`P`, `N`, `A`). Full machine evidence also
retains baseline, minimum, relative depth, template, width, slopes, area and
noise.

- Valid identical ACK `E1/R6/C0`, `CC -> CC`:
  `START:P:1.000 D0:P:1.000 D1:P:.982 D2:N:0 D3:N:.024 D4:P:1.000 D5:P:.850 D6:N:0 D7:N:.102 PARITY:P:.872 STOP:N:0`.
- BC `E8/R0/C0`, `BC -> BC`:
  `START:P:1 D0:P:1 D1:P:1 D2:N:0 D3:N:0 D4:N:0 D5:N:0 D6:P:.956 D7:N:0 PARITY:N:0 STOP:N:0`.
- 9C `E8/R4/C0`, `9C -> 9C`:
  `START:P:1 D0:P:1 D1:P:1 D2:N:0 D3:N:0 D4:N:0 D5:P:1 D6:P:.840 D7:N:0 PARITY:P:1 STOP:N:0`.
- Analog-pulse-missed signature `E1/R5/C0`, `E7 -> 00`: every START/DATA/
  PARITY slot scores `P:1`, STOP `N:0`; parity becomes valid only while all
  eight DATA bits change. This is not a credible parity-only repair.
- Historical valid -> ambiguous `E1/R2/C0`, `CC -> CC`: D5 is `A:.729`;
  byte/parity/STOP agree but strict physical output correctly stays ambiguous.
- Historical valid -> different `E1/R0/C1`, `DF -> 1E`: multiple DATA slots
  change and STOP is pulse-like; this is a clear guardrail failure.
- Physically-corrupted signature `E1/R0/C0`, `FF -> 6E`: multiple DATA changes
  and pulse-like STOP remain visible rather than being protocol-corrected.
- No-physical-evidence signature `E8/R11/C8`, `FF -> FF`: DATA slots are all
  no-pulse, but parity remains physically no-pulse and invalid.

There is no convincing historical-invalid -> V6-valid example whose validity
comes from an isolated physical parity correction without DATA alteration.

## Robustness with frozen structure

| Transform | Slot class | Strict state | Best byte | Record |
|---|---:|---:|---:|---:|
| offset -100 | 100.00% | 100.00% | 100.00% | 100.00% |
| offset +100 | 100.00% | 100.00% | 100.00% | 100.00% |
| gain 0.9 | 99.99% | 99.84% | 100.00% | 99.49% |
| gain 1.1 | 99.91% | 99.52% | 100.00% | 99.49% |
| deterministic noise sigma 5 | 99.34% | 95.34% | 100.00% | 90.86% |

Unlike V5, best-evidence bytes are fully stable under these transforms when
START/grid/boundaries are frozen. Ambiguity thresholds make strict character
and record classifications somewhat more sensitive to noise.

Leave-one-event-out template validation:

- Event 8: slot agreement `99.83%`, strict-state agreement `98.64%`, byte
  agreement `100%`, boundaries `54/54`;
- Event 9: slot agreement `99.81%`, strict-state agreement `98.34%`, byte
  agreement `100%`, boundaries `62/62`.

## Answers and conclusion

A. Strict exact preservation of historically valid characters: **49.02%**
(`56.58%` if only the best-evidence byte is considered, regardless of protocol
validity/ambiguity).

B. Parity-invalid characters made coherent by independent parity evidence with
unchanged DATA: **2**.

C. Historical parity-invalid characters becoming explicitly ambiguous: **42**.

D. Historical parity-invalid characters whose V6 best-evidence DATA changes:
**227/241**.

E. BC is preserved strictly in both occurrences. All 9C bytes are preserved;
three are strict and nine retain physical ambiguity.

F. Offset/gain robustness is excellent and noise robustness is high at slot/
byte level with fixed structure.

This is **case 3**: the current relative/template model is robust to analog
transformations and useful as diagnostic evidence/confidence, but it also
deteriorates too many historically certain characters. The dominant problem is
false pulse evidence in historically no-pulse DATA slots. V6 must not replace
FieldCandidate and is not integrated into Studio. Further work would require a
better independently trained slot model and multi-site captures, not tuning on
parity/checksum/BC/9C outcomes.
