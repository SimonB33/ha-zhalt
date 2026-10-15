#include "executor.h"

#include <Arduino.h>

#include <algorithm>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "passthrough.h"
#include "session.h"
#include "store.h"
#include "zconversation.h"

using z::JobType;
using St = z::Conversation::Status;

static const char* jobName(JobType t) {
  switch (t) {
    case JobType::Mist: return "mist";
    case JobType::Pulse: return "pulse";
    case JobType::Stop: return "stop";
    case JobType::Refresh: return "refresh";
    case JobType::Scheduler: return "scheduler";
    case JobType::TestPump: return "test_pump";
    case JobType::TestLed: return "test_led";
    case JobType::TestBuzzer: return "test_buzzer";
    case JobType::TestJoin: return "test_join";
    case JobType::Passthrough: return "passthrough";
  }
  return "?";
}

static const char* gateResult(z::Gate g) {
  switch (g) {
    case z::Gate::ClockNotSet: return "clock_not_set";
    case z::Gate::Blackout: return "blackout";
    case z::Gate::PulseActive: return "pulse_active";
    case z::Gate::StopPending: return "stop_pending";
    case z::Gate::StopFailed: return "stop_failed";
    default: return "ok";
  }
}

static z::Goal goalFor(JobType t) {
  switch (t) {
    case JobType::Mist: return z::Goal::Mist;
    case JobType::Pulse: return z::Goal::Pulse;
    case JobType::Stop: return z::Goal::Stop;
    case JobType::Scheduler: return z::Goal::Scheduler;
    case JobType::TestPump: return z::Goal::TestPump;
    case JobType::TestLed: return z::Goal::TestLed;
    case JobType::TestBuzzer: return z::Goal::TestBuzzer;
    default: return z::Goal::Refresh;
  }
}

static void absorbUnitData(const z::Conversation& c) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastContact = net::now();
  if (c.haveDat()) {
    const z::Dat& d = c.lastDat();
    app.snap.unitValid = true;
    app.snap.unitOp = d.op_mode;
    app.snap.unitActiveCycle = d.active_cycle;
    char buf[24];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", d.dev_year, d.dev_month, d.dev_day, d.dev_hour,
             d.dev_minute, d.dev_second);
    app.snap.unitClock = buf;
  }
  const z::Settings& s = c.settings();
  if (s.valid) {
    app.snap.unitFirmware = s.firmware_minor;
    app.snap.schedulerOn = z::anyCycleActive(s) ? 1 : 0;
    if (!app.cachedOriginal.valid && z::anyCycleActive(s)) {
      app.cachedOriginal = s;
      store::put("imp", s.raw);
      elog::add("cached unit settings for scheduler restore");
    }
  }
  app.dirty = true;
}

static void recordMist(const char* kind) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastMist = net::now();
  app.snap.lastMistKind = kind;
  store::put("lastmist", std::to_string(app.snap.lastMist));
  store::put("lastkind", kind);
}

static void runConversation(const z::Job& job) {
  z::Conversation conv(goalFor(job.type));
  uint32_t timeoutMs = 30000;
  int secs = 0, minutes = 0;
  z::Settings cached;
  {
    std::lock_guard<std::mutex> l(app.mu);
    if (job.type == JobType::Mist || job.type == JobType::Pulse) {
      z::Gate g = app.safety.canStart(net::now(), net::clockValid());
      if (g != z::Gate::Ok) {
        app.snap.lastResult = gateResult(g);
        app.dirty = true;
        elog::add("%s refused: %s", jobName(job.type), gateResult(g));
        return;
      }
    }
    if (job.type == JobType::Mist) {
      secs = app.safety.clampMistSeconds(job.seconds);
      conv.setMistDurationMs(uint32_t(secs) * 1000);
      timeoutMs = uint32_t(secs) * 1000 + 60000;
    }
    if (job.type == JobType::Pulse) {
      minutes = job.resume ? std::max(1, job.minutes) : app.safety.clampPulseMinutes(job.minutes);
      timeoutMs = 60000;
    }
    if (job.type == JobType::Stop) timeoutMs = 45000;
    if (job.type == JobType::Scheduler) {
      if (job.on && !app.cachedOriginal.valid) {
        app.snap.lastResult = "no_cached_settings";
        app.dirty = true;
        return;
      }
      cached = app.cachedOriginal;
    }
  }
  if (job.type == JobType::Scheduler) conv.setSchedulerTarget(job.on, job.on ? &cached : nullptr);

  bool markedStart = false;
  auto stopNow = [&]() -> bool {
    if (job.type == JobType::Mist && conv.started() && !markedStart) {
      markedStart = true;
      std::lock_guard<std::mutex> l(app.mu);
      app.mistActive = true;
      app.mistEndEpoch = net::now() + secs;
      app.dirty = true;
    }
    if (job.type == JobType::Stop) return false;
    if (app.preempt) return true;
    if (job.type == JobType::Mist) {
      std::lock_guard<std::mutex> l(app.mu);
      return app.safety.inBlackout(net::now());
    }
    return false;
  };

  elog::add("job %s start", jobName(job.type));
  session::Outcome o = session::run(conv, timeoutMs, stopNow);
  {
    std::lock_guard<std::mutex> l(app.mu);
    app.mistActive = false;
    app.mistEndEpoch = 0;
  }
  appRecordJoin(o.joined, o.rssi, o.joinMs);
  int64_t now = net::now();
  bool done = conv.status() == St::Done;
  std::string err = !o.joined                       ? "join_failed"
                    : !o.wsConnected                ? "ws_connect_failed"
                    : conv.status() == St::Running  ? (o.wsDropped ? "ws_dropped" : "timeout")
                                                    : conv.error();
  if (o.wsConnected) absorbUnitData(conv);

  switch (job.type) {
    case JobType::Mist:
      if (conv.started()) recordMist("mist");
      if (done) {
        appSetResult(conv.started() ? "ok" : "cancelled");
      } else {
        // The mist flag may have landed; manual mist has no onboard timeout. Make sure it stops.
        if (o.wsConnected) {
          std::lock_guard<std::mutex> l(app.mu);
          app.safety.requestStop(now);
        }
        appSetResult(err);
      }
      break;
    case JobType::Pulse:
      if (done && conv.started()) {
        {
          std::lock_guard<std::mutex> l(app.mu);
          app.safety.onPulseStarted(now, minutes);
        }
        recordMist("pulse");
        appSetResult("ok");
      } else {
        // The start flag may have landed; the unit may start pulsing late. Make sure it stops.
        if (!done && o.wsConnected) {
          std::lock_guard<std::mutex> l(app.mu);
          app.safety.requestStop(now);
        }
        appSetResult(done ? "cancelled" : err);
      }
      break;
    case JobType::Stop: {
      {
        std::lock_guard<std::mutex> l(app.mu);
        if (done)
          app.safety.onStopConfirmed();
        else
          app.safety.onStopFailed(now);
      }
      appSetResult(done ? "ok" : "stop_attempt_failed:" + err);
      break;
    }
    default:
      appSetResult(done ? "ok" : err);
  }
  elog::add("job %s -> %s", jobName(job.type), done ? "ok" : err.c_str());
}

static void runJob(const z::Job& job) {
  switch (job.type) {
    case JobType::TestJoin: {
      int rssi = 0;
      uint32_t ms = 0;
      bool ok = net::joinZhalt(session::kJoinTimeoutMs, rssi, ms);
      net::leaveZhalt();
      appRecordJoin(ok, rssi, ms);
      appSetResult(ok ? "ok" : "join_failed");
      return;
    }
    case JobType::Passthrough:
      passthrough::run();
      return;
    default:
      runConversation(job);
  }
}

static void executorTask(void*) {
  for (;;) {
    z::Job job;
    bool have;
    {
      std::lock_guard<std::mutex> l(app.mu);
      have = app.queue.pop(job);
    }
    if (!have) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (job.type == JobType::Stop) app.preempt = false;
    app.busy = true;
    app.dirty = true;
    runJob(job);
    app.busy = false;
    app.dirty = true;
  }
}

void executorStart() { xTaskCreatePinnedToCore(executorTask, "executor", 12288, nullptr, 1, nullptr, 1); }
