# Event RAW Logger V1

Baseline validated on the Waveshare ESP32-C6 Touch LCD 1.47 with ADC continuous
on GPIO5 at 83,333 samples/s requested and microSD over SPI at 4 MHz.

## Validated architecture

ADC continuous -> monotonic 64-bit sample index -> 8,333-sample pre-trigger
ring -> artificial event capture -> post-trigger -> SD -> summarized idle mode.
The ADC remains active for the complete session.

## Reference validation

- Duration: 600.096684 s
- Measured rate: 83,319.694 samples/s
- Samples acquired: 49,999,872
- RAW samples stored: 3,499,980
- Intentionally omitted: 46,499,892
- Data loss, DMA overflow, software buffer overflow: 0
- SD errors, short writes, zero writes: 0
- Events: 60
- Per event: 8,333 PRE + 41,667 EVENT + 8,333 POST samples
- PRE-to-live discontinuities and duplicates: 0
- CRC failures after rereading the RAW file: 0
- RAW bytes: 6,999,960
- Total session bytes: 7,021,382
- Session closed cleanly

Accounting invariant:

`49,999,872 = 3,499,980 + 46,499,892 + 0`

The trigger used for this validation is an artificial timer. No analog KNX
trigger, protocol decoder, or bus connection is part of this baseline.
