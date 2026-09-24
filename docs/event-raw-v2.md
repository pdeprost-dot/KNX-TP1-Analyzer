# Event RAW Logger V2

Validated field firmware: `firmware/C6EventRawLogger/C6EventRawLogger.ino` for
the Waveshare ESP32-C6 Touch LCD 1.47.

## Architecture

- continuous ESP-IDF ADC/DMA on GPIO5 at 83,333 samples/s requested;
- monotonic 64-bit sample index;
- experimental `D44 > 500` detector (not a definitive KNX threshold);
- PRE and POST: 8,333 samples each (about 100 ms);
- 12 internal-RAM chunks of 8 KiB, referenced without a PRE copy burst;
- asynchronous selective RAW writer;
- dynamically growing event metadata, without the former fixed
  `EVENT_MAX=1024` ceiling;
- separate DATA_LOSS, DMA, chunk-pool, event-metadata, writer-queue and SD
  counters;
- persistent RAW, chunk map, events, SD incidents and final test result;
- idempotent `RUNNING -> STOP_REQUESTED -> PRODUCER_DRAINED -> WRITER_DRAINED
  -> FINALIZING -> CLOSED` finalization, with `finish()` exactly once.

The format preserves the distinction between `CAPTURED_RAW`,
`SILENCE_SUMMARIZED`, future `INTENTIONAL_GAP` / `PROTECTION_GAP`, and actual
`DATA_LOSS`. Unknown interference must not be classified as silence.

The logger captures analog evidence. KNX protocol interpretation remains a
Studio responsibility; no TP1 decoder runs in this acquisition path.

## SD recovery R1

After a RAW EIO, the writer retains the chunk, closes only the RAW handle,
reopens the same file in append mode, verifies size and position, then retries
that chunk exactly once. R2/R3/R4 stop the session. Each incident is persisted
in `sd-incidents.jsonl` without Serial spam.

**R1 recovery experimentally validated: 3/3 recovered EIO incidents, zero
sample loss and zero RAW discontinuity.**

## 600-second validation

- 600.083071 s; 83,322.011 samples/s; 50,000,128 acquired samples;
- 1,800 events; 37,376,000 RAW samples / 74,752,000 bytes;
- three RAW handle failures, three recovered R1;
- zero DATA_LOSS, DMA overflow, true pool exhaustion, event metadata
  exhaustion, writer queue exhaustion, duplicate, gap, and CRC failure;
- exact accounting invariant; `finish()` once; CLOSED;
- coherent `test-result.json`.

The synthetic scheduler was validation-only and is disabled in field mode.
