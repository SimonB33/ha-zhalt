#include "zconversation.h"

namespace z {

Conversation::Conversation(Goal goal) : goal_(goal) {}

void Conversation::setMistDurationMs(uint32_t ms) { mistMs_ = ms; }

void Conversation::setSchedulerTarget(bool on, const Settings* restoreFrom) {
  schedOn_ = on;
  restoreFrom_ = restoreFrom;
}

void Conversation::requestStop() { stopRequested_ = true; }

std::vector<std::string> Conversation::takeOutgoing() {
  std::vector<std::string> o;
  o.swap(out_);
  return o;
}

void Conversation::onConnected(uint32_t nowMs) {
  if (phase_ != Phase::WaitConnect) return;
  phase_ = Phase::WaitGreeting;
  phaseStartMs_ = nowMs;
}

void Conversation::fail(const std::string& why) {
  if (status_ != Status::Running) return;
  status_ = Status::Failed;
  error_ = why;
  phase_ = Phase::Finished;
}

void Conversation::finish() {
  if (status_ != Status::Running) return;
  status_ = Status::Done;
  phase_ = Phase::Finished;
}

void Conversation::poll(uint32_t nowMs, Action a) {
  out_.push_back(buildPDat(a));
  lastPollMs_ = nowMs;
}

Action Conversation::goalAction() const {
  switch (goal_) {
    case Goal::Mist: return Action::Mist;
    case Goal::Pulse: return Action::Pulse;
    case Goal::Stop: return Action::Stop;
    case Goal::TestPump: return Action::TestPump;
    case Goal::TestLed: return Action::TestLed;
    case Goal::TestBuzzer: return Action::TestBuzzer;
    default: return Action::None;
  }
}

bool Conversation::holdClear(uint32_t nowMs) const {
  return !holdStart_ || nowMs - actSentMs_ >= kStartVerifyMs;
}

void Conversation::beginStop(uint32_t nowMs) {
  holdStart_ = (phase_ == Phase::VerifyStart);  // start flag already sent, spray not seen
  poll(nowMs, Action::Stop);
  lastStopSendMs_ = nowMs;
  phase_ = Phase::VerifyStop;
  phaseStartMs_ = nowMs;
  idleDats_ = 0;
}

void Conversation::enterAct(uint32_t nowMs, const LocalDateTime& clock) {
  phaseStartMs_ = nowMs;
  switch (goal_) {
    case Goal::Refresh:
      phase_ = Phase::Act;
      poll(nowMs, Action::None);
      return;
    case Goal::TestPump:
    case Goal::TestLed:
    case Goal::TestBuzzer:
      phase_ = Phase::Act;
      datsSinceAct_ = 0;
      poll(nowMs, goalAction());
      return;
    case Goal::Stop:
      beginStop(nowMs);
      return;
    case Goal::Mist:
    case Goal::Pulse:
      if (stopRequested_) {  // stop asked before the start was sent: never start
        finish();
        return;
      }
      phase_ = Phase::VerifyStart;
      actSentMs_ = nowMs;
      poll(nowMs, goalAction());
      return;
    case Goal::Scheduler: {
      if (schedOn_ && restoreFrom_ == nullptr) {
        fail("no_cached_settings");
        return;
      }
      if (!schedOn_ && !settings_.valid) {
        fail("no_settings");
        return;
      }
      Settings target = schedOn_ ? *restoreFrom_ : withAllCyclesActive(settings_, false);
      out_.push_back(buildPImpSettings(target, clock));
      lastPollMs_ = nowMs;
      rehandshook_ = false;
      phase_ = Phase::VerifySettings;
      return;
    }
  }
}

void Conversation::onFrame(const std::string& text, uint32_t nowMs, const LocalDateTime& clock) {
  if (status_ != Status::Running) return;
  if (text.compare(0, 4, kGreeting) == 0) {
    if (phase_ == Phase::WaitGreeting) {
      out_.push_back(buildPImpHandshake(clock));
      phase_ = Phase::WaitImp;
      phaseStartMs_ = nowMs;
      lastPollMs_ = nowMs;
      if (goal_ == Goal::Stop) enterAct(nowMs, clock);  // stop does not wait for G_imp
    }
    return;
  }
  if (text.compare(0, 5, "G_imp") == 0) {
    Settings s;
    if (!parseGImp(text, s)) return;
    if (s.valid) settings_ = s;
    if (phase_ == Phase::WaitImp) {
      enterAct(nowMs, clock);
    } else if (phase_ == Phase::VerifySettings && s.valid && anyCycleActive(s) == schedOn_) {
      finish();
    }
    return;
  }
  if (text.compare(0, 5, "G_dat") == 0) {
    Dat d;
    if (parseGDat(text, d)) onDat(d, nowMs);
  }
}

void Conversation::onDat(const Dat& d, uint32_t nowMs) {
  dat_ = d;
  switch (phase_) {
    case Phase::Act:
      if (goal_ == Goal::Refresh) {
        finish();
        return;
      }
      if (++datsSinceAct_ >= 2) finish();
      return;
    case Phase::VerifyStart:
      if (d.spraying()) {
        started_ = true;
        sprayStartMs_ = nowMs;
        if (goal_ == Goal::Pulse) {
          finish();
          return;
        }
        phase_ = Phase::Running;
        idleDats_ = 0;
      }
      return;
    case Phase::Running:
    case Phase::VerifyStop:
      idleDats_ = d.spraying() ? 0 : idleDats_ + 1;
      if (d.spraying() && holdStart_) {  // the pending start has now shown up
        holdStart_ = false;
        started_ = true;
      }
      if (idleDats_ >= kIdleDatsForStop && holdClear(nowMs)) finish();
      return;
    default:
      return;
  }
}

void Conversation::onTick(uint32_t nowMs, const LocalDateTime& clock) {
  if (status_ != Status::Running) return;
  uint32_t inPhase = nowMs - phaseStartMs_;
  bool pollDue = nowMs - lastPollMs_ >= kPollMs;
  switch (phase_) {
    case Phase::WaitConnect:
    case Phase::Finished:
      return;
    case Phase::WaitGreeting:
      if (inPhase >= kGreetingTimeoutMs) fail("no_greeting");
      return;
    case Phase::WaitImp:
      if (inPhase >= kImpWaitMs) {
        enterAct(nowMs, clock);
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::Act:
      if (inPhase >= kStartVerifyMs) {
        fail("no_status");
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::VerifyStart:
      if (stopRequested_) {
        beginStop(nowMs);
        return;
      }
      if (inPhase >= kStartVerifyMs) {
        fail("not_started");
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::Running:
      if (stopRequested_ || nowMs - sprayStartMs_ >= mistMs_) {
        beginStop(nowMs);
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::VerifyStop: {
      // While a pending start may still spray, the deadline runs from the end of the start window.
      bool overdue = holdStart_ ? (holdClear(nowMs) && nowMs - (actSentMs_ + kStartVerifyMs) >= kStopVerifyMs)
                                : inPhase >= kStopVerifyMs;
      if (overdue) {
        fail("stop_not_confirmed");
        return;
      }
      if (nowMs - lastStopSendMs_ >= kStopResendMs) {
        poll(nowMs, Action::Stop);
        lastStopSendMs_ = nowMs;
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    }
    case Phase::VerifySettings:
      if (inPhase >= kSettingsVerifyMs) {
        fail("settings_not_confirmed");
        return;
      }
      if (!rehandshook_ && inPhase >= kRehandshakeMs) {
        out_.push_back(buildPImpHandshake(clock));
        rehandshook_ = true;
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
  }
}

}  // namespace z
