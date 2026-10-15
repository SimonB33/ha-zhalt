#include "app.h"

#include <Arduino.h>
#include <esp_system.h>

#include "eventlog.h"
#include "net.h"
#include "passthrough.h"
#include "store.h"
#include "version.h"

App app;

static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "external";
    default: return "other";
  }
}

void appInit() {
  std::lock_guard<std::mutex> l(app.mu);
  z::Config cfg = z::defaultConfig();
  std::string err, saved = store::get("config");
  if (!saved.empty() && !z::parseConfig(saved, cfg, err)) elog::add("stored config ignored: %s", err.c_str());
  app.safety.setConfig(cfg);
  z::SafetyState st;
  if (z::Safety::deserialize(store::get("safety"), st)) {
    app.safety.restore(st);
    elog::add("safety restored: %s", store::get("safety").c_str());
  }
  app.savedSafety = app.safety.serialize();
  z::Settings s;
  if (z::parseGImp(store::get("imp"), s) && s.valid) app.cachedOriginal = s;
  app.snap.lastMist = atoll(store::get("lastmist").c_str());
  app.snap.lastMistKind = store::get("lastkind");
  app.snap.lastResult = store::get("lastres");
  app.snap.fw = ZB_FW_VERSION;
  app.snap.resetReason = resetReasonStr();
}

void appSubmit(const z::Command& c) {
  using z::CmdType;
  using z::JobType;
  std::lock_guard<std::mutex> l(app.mu);
  z::Job j;
  switch (c.type) {
    case CmdType::Stop:
      app.safety.requestStop(net::now());
      app.preempt = true;
      app.dirty = true;
      return;
    case CmdType::Reboot: app.rebootRequested = true; return;
    case CmdType::Mist: j.type = JobType::Mist; j.seconds = c.seconds; break;
    case CmdType::Pulse: j.type = JobType::Pulse; j.minutes = c.minutes; break;
    case CmdType::Refresh: j.type = JobType::Refresh; break;
    case CmdType::Scheduler: j.type = JobType::Scheduler; j.on = c.on; break;
    case CmdType::TestPump: j.type = JobType::TestPump; break;
    case CmdType::TestLed: j.type = JobType::TestLed; break;
    case CmdType::TestBuzzer: j.type = JobType::TestBuzzer; break;
    case CmdType::TestJoin: j.type = JobType::TestJoin; break;
    case CmdType::Invalid: return;
  }
  if (!app.queue.push(j)) {
    app.snap.lastResult = "queue_full";
    elog::add("queue full, command dropped");
  }
  app.dirty = true;
}

void appApplyConfig(const std::string& json) {
  std::lock_guard<std::mutex> l(app.mu);
  z::Config c = app.safety.config();
  std::string err;
  if (!z::parseConfig(json, c, err)) {
    elog::add("config rejected: %s", err.c_str());
    return;
  }
  app.safety.setConfig(c);
  store::put("config", z::configToJson(c));
  elog::add("config applied");
}

void appTickSafety() {
  std::lock_guard<std::mutex> l(app.mu);
  z::TickResult r = app.safety.tick(net::now(), net::clockValid());
  if (r.action == z::TickAction::Stop) {
    z::Job j;
    j.type = z::JobType::Stop;
    app.queue.push(j);
    app.preempt = true;
    elog::add("safety: stop queued");
  } else if (r.action == z::TickAction::ResumePulse) {
    z::Job j;
    j.type = z::JobType::Pulse;
    j.minutes = r.minutes;
    j.resume = true;
    app.queue.push(j);
    elog::add("safety: resume pulse for %d min", r.minutes);
  }
  std::string s = app.safety.serialize();
  if (s != app.savedSafety) {
    store::put("safety", s);
    app.savedSafety = s;
    app.dirty = true;
  }
}

void appRecordJoin(bool ok, int rssi, uint32_t joinMs) {
  std::lock_guard<std::mutex> l(app.mu);
  int64_t now = net::now();
  app.snap.zhaltReachable = ok;
  if (ok) {
    app.snap.rssiKnown = true;
    app.snap.rssi = rssi;
    app.snap.joinMs = joinMs;
  } else {
    app.joinFailures.push_back(now);
  }
  while (!app.joinFailures.empty() && now - app.joinFailures.front() > 86400) app.joinFailures.pop_front();
  app.snap.joinFailures24h = int(app.joinFailures.size());
  app.dirty = true;
}

void appSetResult(const std::string& result) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastResult = result;
  store::put("lastres", result);
  app.dirty = true;
}

std::string appStateJson() {
  std::lock_guard<std::mutex> l(app.mu);
  z::BridgeSnapshot s = app.snap;
  const z::SafetyState& st = app.safety.state();
  bool mist = app.mistActive;
  s.state = z::deriveState(st, mist);
  s.misting = mist || st.pulseActive;
  s.sessionEnd = mist ? app.mistEndEpoch : (st.pulseActive ? st.pulseEndEpoch : 0);
  s.uptimeS = millis() / 1000;
  s.freeHeap = ESP.getFreeHeap();
  s.ip = net::ethIp().c_str();
  s.passthrough = passthrough::active();
  return z::stateJson(s);
}
