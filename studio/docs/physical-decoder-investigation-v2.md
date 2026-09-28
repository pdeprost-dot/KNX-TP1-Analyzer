# Physical Decoder Investigation V2 (offline)

This study is diagnostic only. It uses the versioned `KNX-F35F14A4` fixture, FieldCandidate `1715/1815`, and the unchanged production physical decoder. No result below corrects bits, changes thresholds, or feeds the KNX semantic parser back into TP1 reconstruction.

## Authoritative baseline

The offline replay remains exact: 2,577 pulses, 622 characters, 197 records; 63 VALID_KNOWN, 14 VALID_UNKNOWN, 98 INVALID_PARITY, 2 INVALID_TIMING, 0 INVALID_CHECKSUM, 8 INCOMPLETE, and 12 ANALOG_UNDECODED. Event 8 remains 990 pulses, 220 characters, 54 records, 19 ACK, and 7 KNX telegrams, including the known BC/9C records.

## Pulse populations

Values below are Q10 / median / Q90. Depth is local-baseline minus minimum ADC; relative depth divides by the local baseline. Width is the extractor's low interval in samples. Template is cosine similarity against the median normalized shape of high-confidence pulses.

| Population | n | depth | relative depth | width | template |
|---|---:|---:|---:|---:|---:|
| Valid telegram pulses | 840 | 660 / 845 / 958 | .288 / .342 / .371 | 2 / 3 / 3 | .746 / .923 / .973 |
| Valid ACK/BUSY pulses | 380 | 664 / 745 / 1,012 | .289 / .317 / .385 | 2 / 3 / 4 | .696 / .846 / .946 |
| Assigned pulses in INVALID_PARITY | 1,257 | 577 / 720 / 835 | .253 / .297 / .330 | 1 / 2 / 3 | .737 / .924 / .964 |
| Known absent slots in valid characters | 670 | 30 / 82 / 174 | .014 / .035 / .071 | — | .518 / .591 / .667 |
| Missing parity-slot candidates | 191 | 34 / 68 / 733 | .015 / .029 / .298 | — | .548 / .658 / .865 |
| Unexpected parity pulses | 50 | 585 / 734 / 810 | .257 / .301 / .337 | 1 / 2 / 4 | .651 / .821 / .945 |

Relative depth and template score each separate certain pulses from known absent slots well on this corpus (AUC .988 and .987). A joint diagnostic cut based on the Q10 of certain pulses marks 44/191 missing parity locations as pulse-like, with 12/670 false positives on known absent slots. Searching all absent data/parity slots finds at least one pulse-like depression in 177/241 invalid-parity characters versus 6/189 valid characters.

This is evidence for weak/missed analog activity in many invalid characters, not proof of the correct bit. At the exact parity slot, none of the 191 candidates crosses the current absolute LOW threshold and none lies inside an already detected low interval. The responsible missed pulse can be in a data slot rather than the parity slot.

Already assigned invalid-record pulses remain strongly pulse-shaped: template similarity does not separate them from certain pulses (AUC .473), although relative depth is lower (AUC .785). A matched filter alone therefore cannot classify character validity.

## Timing, START, and segmentation

Valid-character timing RMS is .228 / .360 / .543 samples; invalid-parity timing RMS is 0 / .576 / 1.216. Maximum errors are .361 / .639 / .917 versus 0 / .958 / 1.958 samples. Sixteen invalid characters contain unassigned pulses, none near the expected parity slot; no duplicate-slot pulse was observed. Twenty-four invalid-parity characters also have invalid STOP.

Of 241 invalid-parity characters, 191 show an expected parity pulse absent and 50 an unexpected parity pulse present. 114 occur at a record boundary. A deliberately permissive alternate-START experiment finds many locally parity/STOP-valid alternatives, including 34/39 interior cases, but the selected offsets are typically 8–18 samples and 36 are approximately one bit. This is classic bit-shift opportunity and is not evidence that the alternate START is correct. Record-level continuity and independent physical evidence are required before considering START replacement.

The two INVALID_TIMING records contain two characters, one unassigned pulse, and two invalid STOP states.

## Counterfactual experiments

Changing only LOW while holding HIGH at 1815 produces large discontinuities:

| LOW | pulses | characters | records | known | unknown | parity invalid |
|---:|---:|---:|---:|---:|---:|---:|
| 1650 | 1,167 | 201 | 93 | 54 | 0 | 25 |
| 1700 | 1,568 | 377 | 194 | 72 | 14 | 91 |
| **1715 baseline** | **2,577** | **622** | **197** | **63** | **14** | **98** |
| 1750 | 4,876 | 743 | 144 | 72 | 51 | 21 |
| 1800 | 5,050 | 749 | 148 | 74 | 70 | 3 |

The apparent growth in valid records at higher LOW is not accepted as improvement: it coincides with roughly twice as many pulses and radically different segmentation, so opportunistic frames are plausible.

Using 83,315 or 83,330 Hz instead of 83,333 Hz with unchanged pulse positions yields 627 characters and 194 records instead of 622/197, while preserving the 63/14 known/unknown records. This exposes a quantization/boundary sensitivity in character completion and record segmentation; it does not demonstrate the correct rate.

## Probable causes and candidate directions

Evidence-ranked, non-exclusive causes:

1. **Weak pulses below LOW in absent bit/parity slots — strong corpus evidence.** The 177/241 versus 6/189 character-level separation is substantial, but multi-site validation is required.
2. **Shallower/shorter pulse population — moderate evidence.** Assigned invalid pulses are shallower and shorter, yet their normalized shapes overlap valid pulses strongly.
3. **Timing dispersion — moderate evidence.** Invalid characters have larger RMS/max errors; 16 contain unassigned pulses.
4. **STOP failure — direct evidence for 24 characters.** This can coexist with other causes.
5. **Wrong START or record boundary — open/low evidence.** Naive alternatives readily manufacture locally valid characters and are therefore unsafe.
6. **Sample-rate/rounding boundary effects — demonstrated sensitivity, unknown causal importance.** Certain telegrams remain stable in the small tested range.
7. **Non-TP1 analog activity — possible for the residual population, not quantified as a dominant cause.**

Candidate algorithms for later evaluation, not implementation:

1. Relative local-baseline depth plus normalized template score as a **diagnostic confidence feature**; strongest evidence, offset/gain resistant by construction.
2. Deterministic weak-pulse candidate reporting per expected slot, retaining raw evidence and never flipping a bit automatically.
3. A physical confidence score combining timing, relative depth, shape, slope, width, START and STOP; diagnostic-only until multi-site corpora exist.
4. Rational/fixed-phase timing accumulation and explicit boundary rules to study sample-rate quantization without semantic feedback.
5. START alternatives only with record-level continuity and independent waveform evidence; current local parity/STOP scoring is rejected as too opportunistic.

No production decoder change is recommended from this single-site corpus. Additional analyzers/frontends/sites are required to choose universal relative thresholds or promote any candidate method.
