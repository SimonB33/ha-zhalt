#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "zprotocol.h"

namespace z {

enum class Goal { Refresh, Mist, Pulse, Stop, TestPump, TestLed, TestBuzzer, Scheduler };

// One WebSocket session with the unit, as a pure state machine.
// Feed it events; send whatever takeOutgoing() returns, in order.
class Conversation {
 public:
  enum class Status { Running, Done, Failed };

  static constexpr uint32_t kPollMs = 1500;
  static constexpr uint32_t kGreetingTimeoutMs = 8000;
  static constexpr uint32_t kImpWaitMs = 4000;
  static constexpr uint32_t kStartVerifyMs = 30000;
  static constexpr uint32_t kStopVerifyMs = 25000;
  static constexpr uint32_t kStopResendMs = 10000;
  static constexpr uint32_t kSettingsVerifyMs = 15000;
  static constexpr uint32_t kRehandshakeMs = 3000;
  static constexpr int kIdleDatsForStop = 2;

  explicit Conversation(Goal goal);
  void setMistDurationMs(uint32_t ms);
  void setSchedulerTarget(bool on, const Settings* restoreFrom);
  void requestStop();

  void onConnected(uint32_t nowMs);
  void onFrame(const std::string& text, uint32_t nowMs, const LocalDateTime& clock);
  void onTick(uint32_t nowMs, const LocalDateTime& clock);
  std::vector<std::string> takeOutgoing();

  Status status() const { return status_; }
  const std::string& error() const { return error_; }
  bool started() const { return started_; }
  const Settings& settings() const { return settings_; }
  const Dat& lastDat() const { return dat_; }
  bool haveDat() const { return dat_.valid; }

 private:
  enum class Phase { WaitConnect, WaitGreeting, WaitImp, Act, VerifyStart, Running, VerifyStop, VerifySettings, Finished };

  void fail(const std::string& why);
  void finish();
  void enterAct(uint32_t nowMs, const LocalDateTime& clock);
  void beginStop(uint32_t nowMs);
  void poll(uint32_t nowMs, Action a);
  void onDat(const Dat& d, uint32_t nowMs);
  Action goalAction() const;
  bool holdClear(uint32_t nowMs) const;

  Goal goal_;
  Phase phase_ = Phase::WaitConnect;
  Status status_ = Status::Running;
  std::string error_;
  std::vector<std::string> out_;
  Settings settings_;
  Dat dat_;
  uint32_t phaseStartMs_ = 0, lastPollMs_ = 0, sprayStartMs_ = 0, lastStopSendMs_ = 0;
  uint32_t actSentMs_ = 0;  // when the Mist/Pulse start flag was sent
  uint32_t mistMs_ = 70000;
  bool stopRequested_ = false, started_ = false, schedOn_ = false, rehandshook_ = false;
  bool holdStart_ = false;  // stop sent while a start was pending, spray not yet seen
  const Settings* restoreFrom_ = nullptr;
  int idleDats_ = 0, datsSinceAct_ = 0;
};

}  // namespace z
