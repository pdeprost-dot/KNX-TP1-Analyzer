#include "analysis.h"
#include "acquisition.h"
#include "storage.h"
#include "web_server.h"
#include "tp1_decoder.h"
#include "hmi.h"

namespace analysis {
namespace {
State current = State::Stopped;
uint32_t session = 0;
uint32_t startedMs = 0;
uint32_t stoppedElapsedMs = 0;
uint32_t sampleBaseline = 0;
uint32_t overrunBaseline = 0;
}

bool start() {
  if (current == State::Starting || current == State::Stopping) return false;
  if (current == State::Running) return scope::start();
  current = State::Starting;
  const auto adc = scope::status();
  sampleBaseline = adc.samples;
  overrunBaseline = adc.overruns;
  startedMs = millis();
  stoppedElapsedMs = 0;
  if (!storage::startSession(webui::errorCount())) {
    current = State::Error;
    return false;
  }
  if (!scope::start()) {
    storage::stopSession(webui::errorCount());
    current = State::Error;
    return false;
  }
  ++session;
  current = State::Running;
  hmi::showScope();
  Serial.printf("{\"type\":\"ANALYSIS\",\"state\":\"RUNNING\",\"session\":%lu}\n", session);
  return true;
}

bool stop() {
  if (current == State::Stopped) return true;
  if (current == State::Starting || current == State::Stopping) return false;
  current = State::Stopping;
  const bool adcOkay = scope::stop();
  const bool tp1Okay = tp1::flush();
  const bool sdOkay = storage::stopSession(webui::errorCount());
  stoppedElapsedMs = millis() - startedMs;
  current = adcOkay && sdOkay && tp1Okay ? State::Stopped : State::Error;
  Serial.printf("{\"type\":\"ANALYSIS\",\"state\":\"%s\",\"session\":%lu}\n", name(current), session);
  return adcOkay && sdOkay && tp1Okay;
}

Status status() {
  const auto adc = scope::status();
  const uint32_t elapsed = (current == State::Running || current == State::Starting)
    ? millis() - startedMs : stoppedElapsedMs;
  return {current, session, elapsed, adc.samples - sampleBaseline, adc.overruns - overrunBaseline};
}

const char *name(State value) {
  switch (value) {
    case State::Stopped: return "STOPPED";
    case State::Starting: return "STARTING";
    case State::Running: return "RUNNING";
    case State::Stopping: return "STOPPING";
    default: return "ERROR";
  }
}
}
