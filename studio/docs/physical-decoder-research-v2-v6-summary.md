# TP1 Physical Decoder Research — V2 to V6

## Scope

These studies replay the versioned offline corpus `KNX-F35F14A4`. They do not
modify firmware, Studio UI, the production `Tp1DeterministicDecoder`, the KNX
Telegram Parser, or the normal Studio decoding path. Experimental V5/V6 code is
isolated under `KNXAnalyzer.Core/Experimental`.

The immutable production reference remains FieldCandidate `1715/1815` on the
tested installation: 2,577 pulses, 622 characters and 197 records. Event 8 has
990 pulses, 220 characters, 54 records, 19 ACKs and 7 parsed telegrams,
including `BC FF 16 00 01 E1 00 80 CA` and
`9C FF 16 00 01 E1 00 80 EA`.

## Successive findings

### V2 — analog and timing investigation

`INVALID_PARITY` errors are strongly correlated with weak analog shapes close
to the absolute threshold. Timing alone does not explain or repair them.

### V3 — local slot evidence

Relative depth and normalized pulse-template evidence provide strong local
discrimination when the slot position is already known. Measured AUC is near
0.99, with excellent offset/gain invariance. This is promising as a local
physical measurement and confidence diagnostic.

### V4 — character timing and phase

Strict phase optimization does not independently repair parity errors. It
repairs no convincing character and increases some unexplained pulses. Analog
extraction/association remains the dominant problem rather than fine phase.

### V5 — free evidence decoder

Using evidence for unrestricted pulse and START discovery fails globally:

- 6,965 definite pulses plus 21,482 ambiguous candidates;
- 1,679 characters and 589 records;
- no parsed telegram;
- BC lost and 9C physically ambiguous.

The local discriminator is not suitable as the current free global detector.

### V6 — fixed FieldCandidate structure with slot evidence

Free START detection is removed: all 622 characters and 197 record boundaries
are frozen from FieldCandidate, while the 11 slots are independently re-read.

- 175/357 historically valid characters remain strictly identical and valid
  (`49.02%`);
- 202/357 best-evidence bytes remain identical (`56.58%`);
- only 2 historical parity-invalid characters are repaired with unchanged DATA;
- 227/241 parity-invalid characters also change DATA;
- both BC occurrences remain strict and valid;
- all twelve 9C bytes remain identical, but nine are physically ambiguous;
- offset/gain/noise robustness remains excellent at slot and byte level;
- the model is too permissive on historically recessive DATA slots.

## Conclusion

V5 and V6 must not be adopted as production decoders. FieldCandidate remains
the physical reference for the tested installation.

Slot Evidence remains useful as:

- a diagnostic tool;
- an independent physical-confidence measurement;
- an analog investigation aid;
- a basis for future research.

Further tuning on this single corpus would risk overfitting. The next credible
scientific step is an authoritative Continuous RAW corpus covering multiple
sites, measurement points and Analyzers. No V7 or multi-site work is started by
this checkpoint.
