#include "zsafety.h"

#include <ArduinoJson.h>

#include "ztime.h"

namespace z {

bool Safety::inBlackout(int64_t now) const {
  int m = localMinuteOfDay(now);
  for (const auto& w : config_.windows)
    if (w.contains(m)) return true;
  return false;
}

Gate Safety::canStart(int64_t now, bool clockValid, bool ignoreBlackout) const {
  if (!clockValid) return Gate::ClockNotSet;
  if (st_.stopFailed) return Gate::StopFailed;
  if (st_.stopPending) return Gate::StopPending;
  if (!ignoreBlackout && inBlackout(now)) return Gate::Blackout;
  if (st_.pulseActive) return Gate::PulseActive;
  return Gate::Ok;
}

int Safety::clampMistSeconds(int requested) const {
  int s = requested > 0 ? requested : config_.mistSeconds;
  if (s < 1) s = 1;
  if (s > kMistCapSec) s = kMistCapSec;
  return s;
}

int Safety::clampPulseMinutes(int requested) const {
  int m = requested > 0 ? requested : kPulseDefaultMin;
  if (m < kPulseMinMin) m = kPulseMinMin;
  if (m > kPulseMaxMin) m = kPulseMaxMin;
  return m;
}

void Safety::onPulseStarted(int64_t now, int minutes) {
  st_.pulseActive = true;
  st_.pulseEndEpoch = now + int64_t(minutes) * 60;
  st_.paused = false;
  st_.pausedRemainingSec = 0;
  st_.stopFailed = false;
}

void Safety::beginStop(int64_t now) {
  st_.stopPending = true;
  st_.stopInFlight = false;
  st_.nextStopAt = now;
  st_.stopAttempts = 0;
  st_.stopFailed = false;
}

void Safety::requestStop(int64_t now) {
  st_.pulseActive = false;
  st_.paused = false;
  st_.pausedRemainingSec = 0;
  beginStop(now);
}

void Safety::onStopConfirmed() {
  st_.stopPending = false;
  st_.stopInFlight = false;
  st_.stopAttempts = 0;
  st_.pulseActive = false;  // a stop that lands always ends the session
}

void Safety::onStopFailed(int64_t now) {
  st_.stopInFlight = false;
  if (st_.stopAttempts > kStopRetries) {
    st_.stopPending = false;
    st_.stopFailed = true;
    st_.pulseActive = false;
    st_.paused = false;
    st_.pausedRemainingSec = 0;
  } else {
    st_.nextStopAt = now + kStopRetryGapSec;
  }
}

TickResult Safety::dispatchStop() {
  st_.stopInFlight = true;
  st_.stopAttempts++;
  return {TickAction::Stop, 0};
}

TickResult Safety::tick(int64_t now, bool clockValid) {
  if (st_.stopPending) {
    if (!st_.stopInFlight && now >= st_.nextStopAt) return dispatchStop();
    return {};
  }
  if (!clockValid) {
    // Without a clock a session cannot be timed, so fail safe: stop it.
    if (st_.pulseActive || st_.paused) {
      requestStop(now);
      return dispatchStop();
    }
    return {};
  }
  if (st_.pulseActive) {
    if (inBlackout(now)) {
      int64_t rem = st_.pulseEndEpoch - now;
      st_.pulseActive = false;
      st_.paused = rem > 0;
      st_.pausedRemainingSec = rem > 0 ? rem : 0;
      beginStop(now);
      return dispatchStop();
    }
    if (now >= st_.pulseEndEpoch) {
      st_.pulseActive = false;
      beginStop(now);
      return dispatchStop();
    }
    return {};
  }
  if (st_.paused && !inBlackout(now)) {
    int64_t rem = st_.pausedRemainingSec;
    st_.paused = false;
    st_.pausedRemainingSec = 0;
    if (rem > 0 && rem >= int64_t(config_.resumeMinMinutes) * 60)
      return {TickAction::ResumePulse, int((rem + 59) / 60)};
  }
  return {};
}

void Safety::restore(const SafetyState& s) {
  st_ = s;
  st_.stopInFlight = false;
  if (st_.stopPending) st_.nextStopAt = 0;  // retry straight away after a reboot
}

std::string Safety::serialize() const {
  JsonDocument d;
  d["pa"] = st_.pulseActive;
  d["pe"] = st_.pulseEndEpoch;
  d["p"] = st_.paused;
  d["pr"] = st_.pausedRemainingSec;
  d["sp"] = st_.stopPending;
  d["na"] = st_.nextStopAt;
  d["sa"] = st_.stopAttempts;
  d["sf"] = st_.stopFailed;
  std::string s;
  serializeJson(d, s);
  return s;
}

bool Safety::deserialize(const std::string& json, SafetyState& out) {
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonObject>()) return false;
  SafetyState s;
  s.pulseActive = d["pa"] | false;
  s.pulseEndEpoch = d["pe"] | int64_t(0);
  s.paused = d["p"] | false;
  s.pausedRemainingSec = d["pr"] | int64_t(0);
  s.stopPending = d["sp"] | false;
  s.nextStopAt = d["na"] | int64_t(0);
  s.stopAttempts = d["sa"] | 0;
  s.stopFailed = d["sf"] | false;
  out = s;
  return true;
}

}  // namespace z
