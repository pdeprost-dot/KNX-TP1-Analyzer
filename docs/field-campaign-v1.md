# Field Campaign V1

Field Campaign V1 adds descriptive metadata and Web preparation around the qualified
`CONTINUOUS_RAW` pipeline. It does not change ADC, DMA, chunking, storage, integrity,
finalization, or the Wi-Fi-off capture lifecycle.

The Web form accepts `site`, `bus`, and `point` up to 64 UTF-8 bytes each, `note` up
to 160 UTF-8 bytes, and a requested duration from 10 to 32,400 seconds for Continuous
RAW. Empty descriptions are valid. START is rejected before scheduling the capture if
the Analyzer is not IDLE/CLOSED, storage is not HEALTHY, SD is unavailable, the mode or
duration is invalid, a text limit is exceeded, or free space is less than the estimated
RAW size plus 16 MiB.

At START the values are frozen in `session-start.json` and the physical authoritative
`manifest.json`:

```json
"field_campaign": {
  "schema": "knx-field-campaign-1.0",
  "site": "Field Campaign Test",
  "bus": "Test Segment",
  "point": "Bench",
  "note": "Field Campaign V1 validation",
  "requested_duration_s": 10
}
```

Studio treats this section as optional. Existing EVENT and CONTINUOUS_RAW sessions
without it remain readable; transitional flat V0.7 labels are also read. Session IDs
remain the only authoritative identity for folders and caches.

The Web UI provides 10, 60 and 300 second presets plus a custom duration. Continuous
RAW does not require a valid TP1 calibration; calibration remains informative. Wi-Fi
is still stopped before ADC capture and restored only after storage finalization.

Validation session `KNX-135D1BFC` completed in 10.135735 s with 831,488 samples,
203 chunks and 1,662,976 RAW bytes. It closed COMPLETE with no loss, gap, DMA, ADC,
SD or pool error and a true invariant. Its physical manifest contains the expected
campaign snapshot and Studio imported it with valid bounded RAW/CRC access.

The first live Studio import received one prematurely ended HTTP metadata response;
one unchanged retry passed. The manifest, metadata, RAW and CRC were valid, and no SD
or RAW corruption was demonstrated. This transport anomaly remains open.

The firmware installed for the 10-second validation was built immediately before the
final UI-only escaping of campaign text in the HTML summary. The versioned final build
includes that escaping; it changes presentation only and was not redeployed solely for
this reason.
