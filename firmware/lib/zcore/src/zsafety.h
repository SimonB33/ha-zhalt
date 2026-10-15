#pragma once
#include <cstdint>
#include <string>

#include "zconfig.h"

namespace z {

struct SafetyState {
  bool pulseActive = false;
  int64_t pulseEndEpoch = 0;
  bool paused = false;  // pulse stopped for a blackout; may resume when it ends
  int64_t pausedRemainingSec = 0;
  bool stopPending = false;
  bool stopInFlight = false;  // not persisted
  int64_t nextStopAt = 0;
  int stopAttempts = 0;
  bool stopFailed = false;
};

enum class TickAction { None, Stop, ResumePulse };
// minutes is set for ResumePulse only: the remaining session time, rounded up.
// Resume minutes must NOT be passed through clampPulseMinutes (a sub-10-minute
// remainder would be raised to 10).
struct TickResult {
  TickAction action = TickAction::None;
  int minutes = 0;
};
enum class Gate { Ok, ClockNotSet, Blackout, PulseActive, StopPending, StopFailed };

class Safety {
 public:
  static constexpr int kMistCapSec = 120;
  static constexpr int kPulseMinMin = 10;
  static constexpr int kPulseMaxMin = 90;
  static constexpr int kPulseDefaultMin = 40;
  static constexpr int kStopRetries = 3;
  static constexpr int kStopRetryGapSec = 20;

  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  Gate canStart(int64_t now, bool clockValid) const;
  bool inBlackout(int64_t now) const;
  int clampMistSeconds(int requested) const;
  int clampPulseMinutes(int requested) const;

  void onPulseStarted(int64_t now, int minutes);
  void requestStop(int64_t now);  // user/HA stop: cancels any session
  void onStopConfirmed();
  void onStopFailed(int64_t now);
  TickResult tick(int64_t now, bool clockValid);  // call about once a second

  const SafetyState& state() const { return st_; }
  void restore(const SafetyState& s);
  std::string serialize() const;
  static bool deserialize(const std::string& json, SafetyState& out);

 private:
  void beginStop(int64_t now);
  TickResult dispatchStop();  // counts one attempt and marks it in flight

  Config config_ = defaultConfig();
  SafetyState st_;
};

}  // namespace z
