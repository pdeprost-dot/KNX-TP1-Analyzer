# Physical Decoder V5 — Parallel Evidence Decoder Prototype

## Scope

Offline experiment on the versioned `KNX-F35F14A4` fixture. The production
`Tp1DeterministicDecoder`, FieldCandidate `1715/1815`, Studio UI and firmware
are unchanged. `Tp1EvidenceDecoder` is isolated under `Core/Experimental` and
is not wired into the product path.

The physical path is deliberately blind to parity, STOP, checksum and KNX
semantics:

`RAW -> local baseline/depth -> normalized template evidence -> fractional
9600 bit/s grid -> PULSE/NO_PULSE/AMBIGUOUS -> characters -> protocol checks`

The template is the median normalized 16-sample shape of pulses belonging to
historically valid records. This use of the historical decoder is restricted
to template training; no record currently being decoded can influence its
physical bits through parity, STOP, checksum or parser results. Leave-one-event-
out runs quantify the remaining corpus dependence for Events 8 and 9.

## Deterministic physical rules

- Local reference: 75th percentile over 41 samples.
- Relative depth reference: event-local 95th percentile of local-minimum depths.
- Evidence: 55% normalized relative depth and 45% normalized template cosine.
- V3 fixed physical anchors: negative/positive depth P90/P10 `0.196/0.823`,
  template `0.637/0.742`.
- `PULSE >= 0.75`, `NO_PULSE <= 0.25`, otherwise `AMBIGUOUS`.
- Candidate suppression: retain the strongest candidate within ±5 samples.
- START: every retained definite physical pulse not already inside the current
  96-sample character window. No protocol feedback is used.
- Grid: `sampleRate / 9600`, fractional expected positions, fixed ±3-sample
  observation window.
- Best-evidence bytes use score `>= 0.5` as pulse, while any ambiguous slot
  keeps the character and record explicitly `AMBIGUOUS`.
- Records use only historical TP1 time gaps. Protocol validation happens after
  physical construction.

## Quantitative comparison

Historical baseline remains exactly 2,577 pulses, 622 characters and 197
records. Event 8 remains 990 pulses, 220 characters, 54 records, 19 ACK and
7 parsed telegrams, including both reference frames.

| Measure | Historical FieldCandidate | Evidence V5 |
|---|---:|---:|
| Definite pulses | 2,577 | 6,965 |
| Ambiguous pulse candidates | — | 21,482 |
| Characters | 622 | 1,679 |
| Parity valid / invalid | — | 1,143 / 536 |
| Invalid STOP | — | 793 |
| Physically ambiguous characters | — | 1,260 |
| Records | 197 | 589 |
| VALID_UNKNOWN | 14 | 10 |
| VALID_KNOWN | 63 | 10 |
| ACK | 62 | 15 |
| BUSY | 1 | 5 |
| INVALID_PARITY | 98 | 12 |
| INVALID_TIMING | 2 | 8 |
| INCOMPLETE | 8 | 0 |
| ANALOG_UNDECODED | 12 | 0 |
| AMBIGUOUS | — | 559 |
| Parsed telegrams | baseline preserved | 0 |

Character matrix (matched within ±5 samples):

- historical valid -> evidence valid/invalid/ambiguous: `111/49/99`;
- historical invalid -> evidence valid/invalid/ambiguous: `59/17/41`;
- unmatched historical characters: `246`.

Record matrix:

- historical valid -> evidence valid/invalid/ambiguous: `10/13/48`;
- historical invalid -> evidence valid/invalid/ambiguous: `0/4/40`;
- unmatched historical records: `82`;
- strong/weak historical-invalid -> evidence-valid candidates: `0/0`.

ACK comparison: 15 common, 47 historical-only, no V5-only ACK. There is no
ACK explosion, but retention is poor.

## Reference and qualitative cases

1. Identical valid record: only 10 historical valid records remain valid at
   the same start; this is insufficient conservation.
2. `BC FF 16 00 01 E1 00 80 CA`: not reconstructed by V5.
3. `9C FF 16 00 01 E1 00 80 EA`: byte sequence appears once, but its record is
   physically ambiguous and has invalid parity despite valid STOP/checksum.
4. Common ACK: 15 ACK starts agree between engines; 47 certain historical ACKs
   are lost.
5. Historical invalid -> evidence valid: no matched strong or weak candidate;
   therefore no convincing repair exists in this run.
6. Historical valid -> evidence invalid/ambiguous: 61 matched records; the
   dominant causes are ambiguous slot evidence and excess/misaligned STARTs.
7. V4 `PHYSICALLY_CORRUPTED` cases do not become credible repairs: protocol
   checks remain adverse or segmentation changes.
8. V4 `NO_PHYSICAL_EVIDENCE` cases remain ambiguous/unmatched rather than being
   fabricated as valid telegrams. This restraint is desirable, but does not
   compensate for the loss of known-good records.

## Robustness and template dependence

| Transform | PULSE | AMBIGUOUS | Characters | Records | START overlap |
|---|---:|---:|---:|---:|---:|
| baseline | 6,965 | 21,482 | 1,679 | 589 | — |
| offset -100 | 6,965 | 21,482 | 1,679 | 589 | 100.00% |
| offset +100 | 6,965 | 21,482 | 1,679 | 589 | 100.00% |
| gain 0.9 | 6,942 | 21,548 | 1,682 | 602 | 95.83% |
| gain 1.1 | 6,971 | 21,481 | 1,676 | 592 | 96.78% |
| deterministic noise sigma 5 | 6,961 | 22,386 | 1,706 | 710 | 62.18% |

Leave-one-event-out remains close to the global template but not identical:

- Event 8: 678 training pulses; 251 characters in both runs; START overlap
  `96.8%`; records `50 -> 49`.
- Event 9: 680 training pulses; 256 characters in both runs; START overlap
  `95.7%`; records `50 -> 49`.

Thus offsets are handled very well and gain reasonably well, but full-decoder
noise stability is poor. Training is not wholly dominated by the tested event,
although measurable template dependence remains.

## Scientific conclusion

For this prototype and corpus, the answer is **no**. Relative/template evidence
is substantially less dependent on absolute ADC offset, but the current blind
candidate/START construction produces 2.7 times as many characters, loses most
known-good records, fails BC, makes 9C ambiguous, and yields no parsed telegram.
It is not at least as credible as FieldCandidate.

The experiment nevertheless isolates the failure upstream of protocol
semantics: a good per-slot discriminator does not by itself provide a reliable
global candidate set or START process. The next hypothesis would be a stronger
purely physical pulse-event/START model, validated on independent captures—not
checksum-driven tuning. No production change or adoption is justified from the
single Analyzer/site corpus.
