# Experimental offline analysis

This branch extends Studio V0 on the PC only. It does not change the firmware or any recorded session.

## Observed traffic

TrafficAnalyzer decodes complete, checksum-valid standard TP1 frames and excludes records marked synthetic_test true from field statistics by default. Synthetic records remain available in the original candidate list and structured exports. Participant and group summaries report only facts derived from recorded bytes: addresses, counts, sessions, APCI service labels, APDU, payload bytes, priority, repeat flag, and checksum state.

Temporal interactions are consecutive decoded telegram pairs in the same session and within a configurable window (100 ms by default). A pattern is reported only after at least two occurrences. Delay statistics describe proximity only; they do not imply command, response, causality, or equipment function.

## Offline RAW pipeline

OfflineRawAnalyzer receives an AnalogSampleStream and never changes its samples. It calculates RAW statistics, a modal resting-level estimate, lower-half-deviation noise RMS, a 32-bin histogram, threshold excursions, edges, pulse widths, and pulse intervals.

The experimental TP1 path uses documented physical timing:

- 9600 bit/s;
- nominal active pulse around 35 µs;
- start bit, eight data bits LSB first, even parity, stop bit;
- 13 bit-times between starts of consecutive frame characters.

The adaptive excursion threshold is the greatest of 12 RAW counts, six times the robust noise estimate, and 8% of peak-to-peak range. Pulse candidates are 15–60 µs to allow for the approximately 12 µs sample spacing in historical captures. Both polarities are tested because the old analog front end is not characterized here. Nearby threshold fragments are collapsed before decoding.

A TP1_VALID_FRAME requires coherent character timing, valid parity and framing for every reconstructed character, a structurally consistent frame length, XOR checksum 0xFF, and acceptance by the generic KNX standard-frame decoder. The result records timing RMS and maximum error plus every classification reason. Activity is never promoted merely because it resembles an expected telegram.

References:

- [KNX TP Telegram Visualiser](https://www.knx.org/projects/knx-tp-telegram-visualiser)
- [KNX System Specifications](https://support.knx.org/hc/en-us/articles/360000040999-KNX-Specifications)

## Limits

Historical captures are isolated triggered windows, not a continuous synchronized ADC stream. A missing decoded frame does not prove the absence of TP1. Analog activity is not correlated with journal participants unless bytes are reconstructed directly from that same RAW signal. The GPIO5 mV display remains experimental; scientific analysis uses RAW.
