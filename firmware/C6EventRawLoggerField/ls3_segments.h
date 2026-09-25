#pragma once

// LS3 owns every SD operation below; it is called only by the writer/finalizer.
constexpr uint64_t LS3_PRODUCTION_SEGMENT_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t LS3_INDEX_STRIDE_CHUNKS = 256;
File segmentsFile, indexFile;
uint64_t ls3SegmentLimit = LS3_PRODUCTION_SEGMENT_BYTES;
uint64_t ls3SegmentBytes = 0, ls3SegmentSampleStart = 0, ls3SegmentSampleEnd = 0;
uint32_t ls3SegmentIndex = 0, ls3SegmentChunks = 0, ls3SegmentCrc = 0, ls3SegmentCount = 0;
uint64_t ls3StoredChunkOrdinal = 0, ls3MapLogicalOffset = 0;
bool ls3SegmentOpen = false;

String ls3RawName(uint32_t index) {
  char name[24];
  snprintf(name, sizeof(name), "raw-%04u.bin", index);
  return String(name);
}
String ls3RawPath(uint32_t index) { return dirPath + "/" + ls3RawName(index); }

bool ls3WriteAll(File &file, const uint8_t *data, size_t length) {
  return file && file.write(data, length) == length;
}

bool ls3OpenSegment(uint64_t sampleStart) {
  const String name = ls3RawName(ls3SegmentIndex);
  raw = SD.open(dirPath + "/" + name, FILE_WRITE);
  if (!raw) return false;
  ls3SegmentBytes = 0;
  ls3SegmentChunks = 0;
  ls3SegmentCrc = 0;
  ls3SegmentSampleStart = sampleStart;
  ls3SegmentSampleEnd = sampleStart;
  ls3SegmentOpen = true;
  segmentsFile.printf("{\"record_type\":\"SEGMENT_OPEN\",\"segment_index\":%u,\"filename\":\"%s\",\"sample_start\":\"%llu\",\"state\":\"OPEN\"}\n",
                      ls3SegmentIndex, name.c_str(), sampleStart);
  segmentsFile.flush();
  return true;
}

bool ls3CompleteSegment(uint64_t sampleEnd) {
  if (!ls3SegmentOpen) return true;
  raw.flush();
  raw.close();
  ls3SegmentSampleEnd = sampleEnd;
  const String name = ls3RawName(ls3SegmentIndex);
  segmentsFile.printf("{\"record_type\":\"SEGMENT_COMPLETE\",\"segment_index\":%u,\"filename\":\"%s\",\"sample_start\":\"%llu\",\"sample_end\":\"%llu\",\"raw_bytes\":\"%llu\",\"chunk_count\":%u,\"crc32\":\"%08X\",\"state\":\"COMPLETE\"}\n",
                      ls3SegmentIndex, name.c_str(), ls3SegmentSampleStart, sampleEnd,
                      ls3SegmentBytes, ls3SegmentChunks, ls3SegmentCrc);
  segmentsFile.flush();
  ++ls3SegmentCount;
  ls3SegmentOpen = false;
  return true;
}

bool ls3Begin(uint64_t limitBytes) {
  ls3SegmentLimit = limitBytes;
  ls3SegmentIndex = ls3SegmentCount = ls3SegmentChunks = ls3SegmentCrc = 0;
  ls3SegmentBytes = ls3StoredChunkOrdinal = ls3MapLogicalOffset = 0;
  ls3SegmentOpen = false;
  segmentsFile = SD.open(dirPath + "/segments.jsonl", FILE_WRITE);
  indexFile = SD.open(dirPath + "/chunk-index.jsonl", FILE_WRITE);
  return bool(segmentsFile) && bool(indexFile);
}

bool ls3PrepareChunk(uint64_t sampleStart, uint32_t rawBytesRequested) {
  if (!ls3SegmentOpen && !ls3OpenSegment(sampleStart)) return false;
  if (ls3SegmentChunks && ls3SegmentBytes + rawBytesRequested > ls3SegmentLimit) {
    if (!ls3CompleteSegment(sampleStart)) return false;
    ++ls3SegmentIndex;
    if (!ls3OpenSegment(sampleStart)) return false;
  }
  return true;
}

void ls3AccountChunk(const Chunk *chunk, size_t bytes) {
  ls3SegmentBytes += bytes;
  ++ls3SegmentChunks;
  ls3SegmentSampleEnd = chunk->sampleStart + chunk->count;
  ls3SegmentCrc = esp_crc32_le(ls3SegmentCrc, (const uint8_t *)chunk->data, bytes);
  ++ls3StoredChunkOrdinal;
}

void ls3WriteSparseIndex(const Chunk *chunk) {
  if ((ls3StoredChunkOrdinal % LS3_INDEX_STRIDE_CHUNKS) != 0) return;
  indexFile.printf("{\"record_type\":\"CHUNK_INDEX\",\"chunk_ordinal\":\"%llu\",\"sample_start\":\"%llu\",\"seq\":\"%llu\",\"chunks_jsonl_offset\":\"%llu\",\"segment_index\":%u,\"segment_offset\":\"%llu\"}\n",
                   ls3StoredChunkOrdinal, chunk->sampleStart, chunk->seq,
                   ls3MapLogicalOffset + mapUsed, ls3SegmentIndex, chunk->rawOffset);
}

void ls3FinishFiles() {
  if (ls3SegmentOpen) ls3CompleteSegment(ls3SegmentSampleEnd);
  if (segmentsFile) { segmentsFile.flush(); segmentsFile.close(); }
  if (indexFile) { indexFile.flush(); indexFile.close(); }
}

bool ls3ExtractU64(const String &line, const char *key, uint64_t &value) {
  String needle = String("\"") + key + "\":";
  int p = line.indexOf(needle); if (p < 0) return false;
  p += needle.length(); while (p < int(line.length()) && (line[p] == '"' || line[p] == ' ')) ++p;
  char *end = nullptr; value = strtoull(line.c_str() + p, &end, 10); return end != line.c_str() + p;
}
bool ls3ExtractCrc(const String &line, uint32_t &value) {
  int p = line.indexOf("\"crc32\":\""); if (p < 0) return false; p += 9;
  char *end = nullptr; value = strtoul(line.c_str() + p, &end, 16); return end != line.c_str() + p;
}

uint32_t ls3VerifyRaw(uint32_t *verifiedSegments = nullptr) {
  File manifest = SD.open(dirPath + "/segments.jsonl", FILE_READ);
  if (!manifest) return 1;
  uint32_t failures = 0, expectedIndex = 0, complete = 0, aggregate = 0;
  uint64_t previousEnd = 0; bool havePrevious = false;
  while (manifest.available()) {
    String line = manifest.readStringUntil('\n');
    if (line.indexOf("\"record_type\":\"SEGMENT_COMPLETE\"") < 0) continue;
    uint64_t index64, bytes, sampleStart, sampleEnd; uint32_t expectedCrc;
    if (!ls3ExtractU64(line,"segment_index",index64) || !ls3ExtractU64(line,"raw_bytes",bytes) ||
        !ls3ExtractU64(line,"sample_start",sampleStart) || !ls3ExtractU64(line,"sample_end",sampleEnd) ||
        !ls3ExtractCrc(line,expectedCrc) || index64 != expectedIndex || (havePrevious && previousEnd != sampleStart)) { ++failures; continue; }
    File part = SD.open(ls3RawPath(uint32_t(index64)), FILE_READ);
    if (!part || uint64_t(part.size()) != bytes) { ++failures; if(part)part.close(); continue; }
    uint8_t buffer[4096]; uint32_t crc = 0;
    while (part.available()) { size_t n=part.read(buffer,sizeof(buffer)); if(!n)break; crc=esp_crc32_le(crc,buffer,n); aggregate=esp_crc32_le(aggregate,buffer,n); }
    part.close(); if (crc != expectedCrc) ++failures;
    previousEnd=sampleEnd; havePrevious=true; ++expectedIndex; ++complete;
  }
  manifest.close();
  if (complete != ls3SegmentCount || aggregate != sessionCrc) ++failures;
  if (verifiedSegments) *verifiedSegments=complete;
  return failures;
}
