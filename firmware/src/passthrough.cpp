#include "passthrough.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "secrets.h"
#include "session.h"

static WiFiServer s_server(81);
static WiFiClient s_pending;
static std::atomic<bool> s_hasPending{false}, s_active{false};
static constexpr uint32_t kIdleLimitMs = 10 * 60 * 1000;

void passthrough::begin() {
#if PASSTHROUGH_ENABLED
  s_server.begin();
#endif
}

bool passthrough::active() { return s_active; }

void passthrough::poll() {
#if !PASSTHROUGH_ENABLED
  return;
#else
  WiFiClient c = s_server.accept();
  if (!c) return;
  if (c.remoteIP().toString() != PASSTHROUGH_ALLOW_IP) {
    elog::add("passthrough rejected %s", c.remoteIP().toString().c_str());
    c.stop();
    return;
  }
  if (s_hasPending || s_active) {
    c.stop();  // one client at a time, like the unit itself
    return;
  }
  s_pending = c;
  s_hasPending = true;
  z::Job j;
  j.type = z::JobType::Passthrough;
  bool queued;
  {
    std::lock_guard<std::mutex> l(app.mu);
    queued = app.queue.push(j);
  }
  if (!queued) {
    s_pending.stop();
    s_hasPending = false;
  }
#endif
}

void passthrough::run() {
  WiFiClient lan = s_pending;
  s_active = true;
  s_hasPending = false;
  app.dirty = true;
  elog::add("passthrough start");
  int rssi = 0;
  uint32_t ms = 0;
  bool joined = false;
  if (lan.connected()) {
    joined = net::joinZhalt(session::kJoinTimeoutMs, rssi, ms);
    appRecordJoin(joined, rssi, ms);
  } else {
    elog::add("passthrough client gone");
  }
  WiFiClient unit;
  if (joined && unit.connect(IPAddress(172, 217, 28, 1), 81, 5000)) {
    uint8_t buf[512];
    uint32_t lastIo = millis();
    while (lan.connected() && unit.connected() && !app.preempt && millis() - lastIo < kIdleLimitMs) {
      int n;
      while ((n = lan.available()) > 0) {
        n = lan.read(buf, n > 512 ? 512 : n);
        if (n <= 0) break;
        unit.write(buf, n);
        lastIo = millis();
      }
      while ((n = unit.available()) > 0) {
        n = unit.read(buf, n > 512 ? 512 : n);
        if (n <= 0) break;
        lan.write(buf, n);
        lastIo = millis();
      }
      delay(2);
    }
  } else if (joined) {
    elog::add("passthrough: unit not reachable");
  }
  unit.stop();
  lan.stop();
  net::leaveZhalt();
  s_active = false;
  app.dirty = true;
  elog::add("passthrough end");
}
