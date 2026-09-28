# Physical Decoder V3 — Slot Evidence study (offline)

This is an experimental, offline-only study of the versioned `KNX-F35F14A4` fixture. It does not feed evidence back into `Tp1DeterministicDecoder`, change FieldCandidate `1715/1815`, or alter any decoded bit. Its labels are references supplied by already-valid FieldCandidate characters, not independent electrical ground truth.

## Method

For each expected START/data/parity/STOP position, the laboratory records event/record/character/slot provenance, theoretical and minimum sample positions, local Q75 baseline, minimum, depth, width, slopes, area, local MAD noise, timing error and normalized-template cosine score. The 16-sample template is the pointwise median of baseline-removed, amplitude-normalized pulses from valid telegram and ACK/BUSY characters. No interpolation is used.

Depth is normalized by the median high-confidence pulse depth in the same event; events without enough reference pulses use the corpus median. This makes the measurement explicitly relative to the local acquisition rather than an ADC code.

The reference populations exclude every `INVALID_PARITY` character:

- **POSITIVE:** 1,220 pulse-bearing slots in valid telegram/ACK/BUSY characters.
- **NEGATIVE:** 670 recessive data/parity slots in valid characters with no FieldCandidate pulse.

The provisional three-state classifier is derived only from these physical distributions. Its transition interval is bounded by NEGATIVE Q90 and POSITIVE Q10: relative depth `.196 → .823`, template score `.637 → .742`. A continuous score uses 55% normalized relative depth and 45% normalized template evidence; `≤.25` is NO_PULSE, `≥.75` is PULSE, and the interval is deliberately AMBIGUOUS. These weights and limits are study parameters, not production constants.

## Internal separation

Values are Q10 / median / Q90.

| Reference | n | Relative depth | Template | Combined |
|---|---:|---:|---:|---:|
| POSITIVE | 1,220 | .823 / 1.000 / 1.172 | .742 / .893 / .969 | .963 / 1.000 / 1.000 |
| NEGATIVE | 670 | .037 / .091 / .196 | .452 / .558 / .637 | 0 / 0 / .073 |

Separation AUC is `.9850` for relative depth, `.9906` for template alone, and `.9896` for the deliberately simple combination. The three-state result is:

- POSITIVE: 1,196 PULSE, 24 AMBIGUOUS, 0 NO_PULSE;
- NEGATIVE: 12 PULSE, 4 AMBIGUOUS, 654 NO_PULSE.

The combination does not improve AUC on this corpus; its value is explainability and an explicit ambiguous region. AUC must not be interpreted as cross-hardware proof because both references originate from one analyzer/site and from the baseline decoder.

## Investigated failures

### Missing parity pulses

The 191 exact parity positions that do not cross LOW contain:

- **61 strong PULSE evidence**;
- **4 AMBIGUOUS**;
- **126 NO_PULSE**.

Their relative-depth Q10/median/Q90 is `.044/.104/.889`, template `.498/.600/.853`, and absolute timing error `.125/1.875/2.875` samples. This is a bimodal-looking population: some locations contain a physically pulse-like depression that remains invisible to the absolute LOW threshold, while most exact parity positions do not. The latter do not prove that the waveform is non-TP1: the parity discrepancy may originate in a missed data slot or an incorrect character boundary.

Representative strong case: `E1/R5/C0/PARITY`, expected sample `11114.125`, minimum `11114`, depth `697`, relative depth `.944`, template `.827`, width 3, fall/rise `621/935`, timing `-.125`, combined `1.000`. FieldCandidate reports no pulse.

Ambiguous case: `E9/R27/C3/PARITY`, depth `546`, relative `.689`, template `.677`, width 3, timing `-.125`, combined `.601`.

No-evidence case: `E1/R0/C0/PARITY`, depth `33`, relative `.045`, template `.522`, combined `0`; its minimum is `2.875` samples late.

### Unexpected parity pulses

Of 50 FieldCandidate pulses where the parity equation expects absence, **47 classify PULSE and 3 AMBIGUOUS; none classify NO_PULSE**. Their relative-depth median is `.877`, template median `.810`, and timing median `.875` sample. They predominantly look like real TP1-shaped pulses, not weak noise or obvious false threshold crossings. This points away from “remove false pulses” as a general repair and toward a wrong preceding bit, START, or character association for these cases.

Representative: `E1/R7/C1/PARITY`, relative `.956`, template `.841`, width 3, timing `-1.125`, combined `1.000`.

### Invalid STOP

Of 24 invalid STOP positions, **23 classify PULSE and 1 AMBIGUOUS**. Valid STOP positions have relative-depth median `.033`, template `.459`, combined `0`; invalid STOP positions have `.830`, `.925`, and `1.000`. Thus invalid STOPs contain real pulse-like activity at the expected STOP time. The evidence is more consistent with character phase/boundary/START association, or a pulse belonging to a neighboring structure, than with small noise.

Representative: `E2/R3/C0/STOP`, relative `.974`, template `.923`, width 3, timing `.195`, combined `1.000`.

## Template and event dependence

Events 8/9 contribute 1,082 of 1,220 positive references. Their relative-depth median is `1.000` and template median `.906`; other events are `1.002` and `.818`. Per-event median templates have cosine `.761–.967` against the global template for small events, while Events 8 and 9 are both `1.000`. The pulse family is recognizably similar, but the current global template is dominated by Events 8/9 and small-event estimates are noisy (often only 6–18 pulses). Template stability outside Events 8/9 is therefore promising but not proven.

## Synthetic robustness

All transforms are memory-only. The template and thresholds remain those learned from the original reference; event pulse-depth references are recomputed from transformed high-confidence pulses.

| Transform | Three-state agreement | Score rank correlation | Absolute LOW agreement |
|---|---:|---:|---:|
| Offset −100 RAW | 100.00% | 1.0000 | 96.52% |
| Offset +100 RAW | 100.00% | 1.0000 | 49.19% |
| Gain 0.9 around baseline | 100.00% | .9997 | 93.60% |
| Gain 1.1 around baseline | 99.95% | .9992 | 96.52% |
| Gaussian noise σ≈5 RAW | 99.40% | .9906 | 99.30% |

The relative evidence is substantially more stable than testing the local minimum against absolute LOW, especially for positive offset. This is an internal invariance result, not proof against real frontend distortions, clipping, bandwidth changes, common-mode effects, or non-Gaussian bus noise.

## Answers and limits

1. POSITIVE high-confidence reference: **1,220**.
2. NEGATIVE high-confidence reference: **670**.
3. Relative-depth separation: **AUC .9850**.
4. Template-only separation: **AUC .9906**.
5. Combined separation: **AUC .9896**; no gain over template alone.
6. Relative classification is invariant to ±100 offset and 0.9 gain here, changes 1/2,155 at gain 1.1 and 13/2,155 under σ≈5 noise.
7. Missing parity: **61 PULSE / 4 AMBIGUOUS / 126 NO_PULSE**.
8. Unexpected parity: **47/50 strongly resemble reference pulses**, 3 ambiguous.
9. Invalid STOP: **23/24 strongly pulse-like**, favoring alignment/boundary explanations over weak noise.
10. Events 8/9 templates are stable with each other; other-event evidence is limited by small samples and cosine `.761–.967`.
11. Events 8/9 have higher template median (`.906` vs `.818`) but similar normalized depth; dominance in the corpus is a confounder.
12. A three-state decision can be defined without absolute ADC LOW on this corpus, using normalized depth and shape, but cannot yet be promoted to production.
13. Hardware-dependent candidates: raw amplitude/offset, pulse-depth reference, sample-window sizes, frontend bandwidth and pulse template details.
14. Potentially portable concepts: bit-relative slot positions, local normalization, normalized shape correlation, continuous confidence and an explicit AMBIGUOUS state.
15. Principal limitation: one capture, analyzer, analog frontend, installation and activity distribution, with reference labels inherited from FieldCandidate rather than independent electrical truth.

## Candidate future architectures (not implemented)

1. **Relative/adaptive Schmitt detector.** Low CPU and memory; directly replaces absolute levels with local baseline/amplitude references. Risk: baseline contamination and inability to distinguish pulse-shaped interference. Evidence: strong relative-depth separation and excellent synthetic invariance, but no multi-site proof.
2. **Slot-aware template detector.** Evaluate a normalized waveform only at timing-allowed slots. Moderate CPU, small fixed template/window memory; rejects non-pulse shapes and preserves a continuous score. Risks: dependence on START/phase and frontend-specific pulse shape; the global template is dominated by Events 8/9. Evidence: best single-feature AUC on this corpus.
3. **Hybrid relative threshold + template + confidence.** Relative detector proposes candidates; slot timing and template shape assign PULSE/NO_PULSE/AMBIGUOUS. Moderate deterministic cost and best diagnostic transparency. Risks: more parameters, correlated reference labels, and unsafe bit correction without record-level continuity. Current evidence supports further offline/multi-hardware study, not production integration.

## Non-regression

The unchanged FieldCandidate baseline remains exactly 2,577 pulses, 622 characters and 197 records. Event 8 remains 990 pulses, 220 characters, 54 records, 19 ACK and 7 telegrams; `BC FF 16 00 01 E1 00 80 CA` and `9C FF 16 00 01 E1 00 80 EA` remain present. No V3 evidence is consumed by production decoding.
