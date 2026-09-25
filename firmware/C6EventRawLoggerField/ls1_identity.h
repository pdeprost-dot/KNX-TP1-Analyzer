#pragma once

#include <Arduino.h>

bool ls1IdentityBegin();
void ls1PrepareSession();
bool ls1WriteSessionStart(const String &directory, uint32_t sampleRate, uint32_t d44Threshold,
                          uint32_t preSamples, uint32_t postSamples,
                          uint32_t chunkSamples, uint32_t chunkCount, uint64_t segmentMaxBytes);
const char *ls1AnalyzerId();
const char *ls1SessionId();
