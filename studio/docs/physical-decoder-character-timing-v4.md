# Physical Decoder V4 — Character timing and alignment study

This study is offline-only on `KNX-F35F14A4`. It reuses V3 Slot Evidence and leaves FieldCandidate `1715/1815`, production segmentation, classifications and KNX parsing untouched. Phase is estimated without parity, STOP, checksum or semantic-parser feedback; those fields are evaluated only after the estimate is frozen.

## Blind estimator

The grid uses the stored `83333 Hz` and exact fractional period `sampleRate / 9600`. For each baseline character, phase candidates are scored from START and D0–D7 only. The cost compares continuous V3 evidence with the already observed physical pulse/no-pulse assignments, plus a very small zero-phase tie-breaker. PARITY and STOP are excluded.

An initial permissive search over ±3 samples was rejected by its independent controls: invalid-character median absolute correction became 2.5 samples, valid parity degraded in 4 cases, invalid parity degraded in 72 cases, and strong unexplained pulses rose from 16 to 165. It was classic opportunistic realignment.

The retained diagnostic search is bounded to ±1.5 samples, consistent with the valid-character control (P95 `1.125`). This bound is experimental and must not become a universal constant.

## Phase distributions

Values are Q10 / median / Q90 in samples.

| Population | n | Signed correction | Absolute correction | P95 absolute |
|---|---:|---:|---:|---:|
| Valid characters | 189 | 0 / 0 / .500 | 0 / 0 / .625 | 1.125 |
| INVALID_PARITY characters | 241 | −1.000 / 0 / 1.125 | 0 / .250 / 1.375 | 1.500 |

The valid control remains stable: 189/189 blind parities and 189/189 STOPs remain valid. Only four valid characters show a material physical-cost improvement; none outside Events 8/9 does.

For INVALID_PARITY, physical cost improves materially for 30 characters and is unchanged for 211. None is mathematically degraded because the original phase remains a candidate. Independent validation, however, does **not** improve: blind parity changes from 147/241 to 141/241, with zero repairs and six degradations; STOP remains 185/241. Strong unexplained pulses increase from 16 to 24. Therefore even the strict phase optimizer does not provide evidence for phase correction as the primary solution.

## V3 populations after blind phase

### 61 missing-parity positions with PULSE evidence

- blind parity: 47 valid before, 44 after; zero improvements, three degradations;
- 48 were already centered within ±1.5 samples and 48 remain centered;
- 18 receive a correction ≥.75 sample;
- no candidate fits a neighboring slot better;
- 57/61 characters also have disagreement between V3 evidence and at least one baseline data slot;
- only 9 show a meaningful START+D0–D7 cost reduction.

These are predominantly analog-detection/bit-assignment problems, not parity pulses rescued by moving the grid. Forty-three satisfy the conservative `ANALOG_PULSE_MISSED` diagnostic rule.

### 126 missing-parity positions with NO_PULSE evidence

- blind parity: 64 valid before, 62 after; zero improvements, two degradations;
- 39 are centered before and 52 after, without parity benefit;
- 56 have stronger evidence in a neighboring slot;
- 112/126 contain at least one data-slot evidence disagreement;
- only 13 materially reduce physical cost.

No pulse should be manufactured at these parity positions. The dominant evidence lies in other data-slot disagreements; 60 retain `NO_PHYSICAL_EVIDENCE` under the conservative final diagnostic, while the remainder stay mostly indeterminate.

### 47 unexpected parity pulses with PULSE evidence

- blind parity: 33 valid before, 32 after; zero improvements, one degradation;
- 36 are already centered before adjustment, 32 after;
- only one fits a neighboring slot better;
- all 47 characters also contain a data-slot evidence disagreement.

These pulses remain physically credible at the parity location. Small phase correction does not turn them into DATA7, STOP, or next START evidence.

### 23 pulse-like invalid STOPs

- 0 match the next character START within three samples;
- 0 disappear as STOP pulses after strict phase adjustment;
- all 23 remain pulse-like at the current STOP;
- six total invalid characters are START-suspect; the STOP population is not explained by a systematic alternate START.

The current corpus cannot distinguish a genuinely corrupted character from a wrong higher-level character boundary for every case, but it disproves the simple “STOP pulse is actually next START” explanation here.

## Diagnostic classification

The deliberately conservative, ordered rules classify the 241 invalid-parity characters as:

| Diagnostic | Count | Evidence level |
|---|---:|---|
| ANALOG_PULSE_MISSED | 43 | moderate/strong on this corpus |
| PHASE_MISALIGNED | 0 | no blind independent validation |
| START_SUSPECT | 4 | weak; no alternate START selected |
| CHARACTER_BOUNDARY_SUSPECT | 5 | weak/moderate |
| PHYSICALLY_CORRUPTED | 23 | moderate: contradictory pulse/STOP evidence remains |
| NO_PHYSICAL_EVIDENCE | 60 | direct at investigated slots |
| INDETERMINATE | 106 | intentionally unresolved |

The broader flags are 6 START suspects and 43 boundary-position suspects; most boundary flags lack enough independent evidence to receive the final boundary classification.

## Record continuity and events

Across 425 adjacent-character pairs, phase delta Q10/median/Q90 is `−1.125/0/1.125`; absolute delta is `0/.250/1.500`. Baseline inter-character gap is `8/17/35.6` samples. There is no demonstrated stable cumulative record phase, so independent character synchronization remains safer than enforcing a record-wide phase. A future soft continuity prior remains speculative.

For valid characters, Events 8/9 have absolute correction `0/0/.500` versus `0/.125/1.075` elsewhere, but the other-event population is only 23. For invalid characters the distributions are similar: `0/.250/1.375` for Events 8/9 and `0/.125/1.250` elsewhere. No timing rule was calibrated only on Events 8/9.

## Sample rate

The fixture exposes stored `83333 Hz`; no independently measured reliable rate exists for this historical event session. Counterfactual grids at 83315, 83330 and 83333 Hz all produce, at reported resolution, identical 622-character distributions: absolute phase `0/0/1.250` and physical cost `0/1.556/5.000`. This narrow rate variation does not explain the failures.

## Answer to the V4 question

The remaining errors are **not primarily repaired by small per-character phase movement**. The valid control is already centered and stable. On invalid characters, strict blind phase adjustment repairs no independent parity, repairs no STOP, slightly degrades parity, and increases unexplained pulses. The strongest supported issue remains analog extraction/slot decision: credible subthreshold pulses, credible “unexpected” pulses, and widespread data-slot disagreements. Some characters are genuinely contradictory or boundary-suspect, but START/boundary hypotheses are not proven for most of them.

## Candidate future pipeline

Evidence-supported stages are: local baseline → relative analog candidates → normalized template evidence → fractional 9600-bit grid → per-slot PULSE/NO_PULSE/AMBIGUOUS → parity/STOP as independent validation.

Still speculative are: replacing the START, imposing record-level phase continuity, automatically correcting ambiguous bits, and universalizing V3 thresholds/templates across hardware. A future implementation should keep phase local and fractional, but only move it when multiple independent physical observations and continuity agree; the present optimizer must not be promoted.

## Non-regression

The unchanged FieldCandidate baseline remains 2,577 pulses, 622 characters and 197 records. Event 8 remains 990 pulses, 220 characters, 54 records, 19 ACK and 7 telegrams. Both known BC/9C telegrams remain present. No V3/V4 decision enters production decoding.
