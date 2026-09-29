# HTTP Range transport V0.8.1

The Session API streams requested file ranges from SD through a fixed 4096-byte read buffer. Range length is independent of that buffer size: every read block is sent completely before the next block is read, with bounded retries and a five-second no-progress timeout. Lightweight counters expose requested and accepted bytes, short writes, zero writes and timeouts through `/api/analyzer`.

`4080` bytes was an empirical workaround for the previous transport behavior, not a protocol requirement. Before the fix, 8192-byte ranges were intermittent and 16384-byte ranges failed while headers could still advertise the complete body. After the fix, 4080, 4096, 8192, 10000 and 16384-byte ranges each passed five repetitions with no byte mismatch. A complete 9,988,096-byte session download using 64 KiB ranges completed in 153 requests, with no retry, in 79.215 seconds and matched SHA-256 `7A47C00024744004DC0002A33CDAA59495B6B9AD4FCB5D52F9EDD07CFDE0B3BD`.

Clients must continue to validate response length, chunk CRC and content hash, and resume only after the last completely validated range. This fix does not alter ADC, DMA, acquisition, SD RAW writes, chunks, CRC, Continuous RAW lifecycle or the Wi-Fi-off capture policy.
