# KNX Telegram Parser V1

This parser is a Core layer placed strictly after deterministic TP1 reconstruction. It receives record bytes and their TP1 classification; it has no access to ADC samples, D44, pulses, thresholds, or waveform state. Its output never feeds back into physical decoding.

## Supported structure

For a standard TP1 record, the parser preserves the original bytes and exposes:

- control field, priority, repeat and system-broadcast flags;
- source as a 16-bit individual address (`area.line.device`);
- destination as either a 16-bit individual address or a raw 16-bit group address, with both two-level and three-level group representations;
- NPCI group flag, hop count, and length;
- TPCI type and sequence number where present;
- raw 10-bit APCI plus known service name when implemented;
- raw APDU, embedded six-bit data, and any additional payload bytes;
- received and calculated checksum.

The checksum rule is the repository's validated TP1 rule: XOR of every record byte is `0xFF`. The calculated checksum is `0xFF XOR` every byte preceding it. Normal malformed bus data returns `Invalid`, `Unsupported`, or `NotApplicable`; it does not throw.

Known APCI names include GroupValueRead, GroupValueResponse, GroupValueWrite and the additional fixed codes represented by the implementation. Unknown values remain `Unknown (raw=...)`. V1 never infers a datapoint type or a business meaning; the UI therefore reports `DPT Unknown`.

One-byte `CC`, `0C`, and `C0` records are represented separately as ACK, NAK, and BUSY bus controls. They are not given fictitious source, destination, TPCI, or APCI fields. TP1 records classified INVALID_PARITY, INVALID_TIMING, INVALID_CHECKSUM, INCOMPLETE, or ANALOG_UNDECODED remain TP1 diagnostics and are not parsed semantically.

## Real reference records

`BC FF 16 00 01 E1 00 80 CA`:

- `BC`: standard control; Low priority; original/non-repeated under the implemented repeat-bit interpretation;
- `FF 16`: source `15.15.22`;
- `00 01`: raw destination `0x0001`, group `0/0/1` (three-level) or `0/1` (two-level);
- `E1`: group destination, hop count 6, NPCI length 1;
- `00 80`: unnumbered-data TPCI, raw APCI `0x080`, GroupValueWrite, embedded data `0x00`, no extra payload;
- `CA`: valid checksum.

`9C FF 16 00 01 E1 00 80 EA` has the same addresses, transport/application fields, and raw application data. Only control bit `0x20` differs (`9C` marks a repeated frame), with the corresponding checksum change from `CA` to `EA`.

The names above describe protocol structure only. In particular, the embedded value is not labelled ON/OFF and destination `0/0/1` receives no equipment or room name without ETS metadata.

## Real-session replay

The deterministic offline regression fixture `tests/KNXAnalyzer.Core.Tests/Fixtures/KNX-F35F14A4.zip` was derived from the CRC-validated local cache of real session `KNX-F35F14A4`. It contains only the 120 event RAW chunks assembled as the original 983,040-byte segment plus the session, segment, chunk, event, and result metadata required by `EventRawV2Reader`. It is an experimental non-regression corpus for this measured installation, not a universal KNX ground truth.

The offline replay uses FieldCandidate `1715/1815` and preserves the physical baseline exactly: 2,577 pulses, 622 characters, and 197 records (14 VALID_UNKNOWN, 63 VALID_KNOWN, 62 ACK, 1 BUSY, 98 INVALID_PARITY, 2 INVALID_TIMING, 0 INVALID_CHECKSUM, 8 INCOMPLETE, 12 ANALOG_UNDECODED).

Semantic parsing yields 14 telegrams, all group-destination GroupValueWrite records, with source `15.15.22` and destination `0/0/1`. Together with 63 successful bus controls, parse statuses are Success 77 and Invalid 120; Unsupported and NotApplicable are zero for this corpus.

This corpus validates the implemented interpretation for the observed records only. Broader priority/control variants, extended frames, uncommon TPCI/APCI services, and authoritative DPT interpretation require additional KNX reference material and independent fixtures.

This offline regression test never constructs `NetworkImportService` and requires no Analyzer, Wi-Fi, HTTP, IP address, or Session API. Live Session API listing, manifests, HTTP Range/resume, CRC transport, and network import remain separate integration concerns and are not prerequisites for Core decoder/parser validation.

## Human visual validation

The Event 8 Studio UI was validated manually against the real waveform and reconstructed records:

- original telegram `BC FF 16 00 01 E1 00 80 CA`: standard, Low priority, non-repeated, source `15.15.22`, group destination `0/0/1`, unnumbered-data TPCI, GroupValueWrite APCI `0x080`, embedded `0x00`, and valid parity;
- repeated telegram `9C FF 16 00 01 E1 00 80 EA`: the same semantic fields and application data, with Repeat set;
- ACK `CC`: presented as a one-byte bus control with no invented source, destination, TPCI, APCI, APDU, or DPT;
- short `C8` INVALID_PARITY and long `FF FF FF FB DF BF FF AF` INVALID_PARITY: retained as physical diagnostics, with no semantic `KnxTelegram` fabricated;
- Character / Slot Diagnostic: character identity, LSB-first bits, expected/observed parity, STOP, timing errors, unassigned and duplicate-slot pulses, slot/sample positions, and associated pulses were all visually confirmed.

No semantic parser is applied to invalid TP1 records or bus controls. DPT intentionally remains Unknown without external datapoint metadata. This validation does not claim coverage of extended frames, every TPCI/APCI service, or every KNX installation.
