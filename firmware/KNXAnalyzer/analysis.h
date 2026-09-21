#pragma once

#include <Arduino.h>

namespace analysis {
enum class State : uint8_t { Stopped, Starting, Running, Stopping, Error };
struct Status {
  State state;
  uint32_t session;
  uint32_t sessionElapsedMs;
  uint32_t sessionSamples;
  uint32_t sessionOverruns;
};
bool start();
bool stop();
Status status();
const char *name(State state);
}
