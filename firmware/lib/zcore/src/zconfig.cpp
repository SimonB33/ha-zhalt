#include "zconfig.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace z {

bool Window::contains(int m) const {
  if (!enabled()) return false;
  if (startMin < endMin) return m >= startMin && m < endMin;
  return m >= startMin || m < endMin;
}

Config defaultConfig() {
  Config c;
  c.windows[0] = Window{5 * 60 + 55, 6 * 60 + 15};
  c.windows[1] = Window{7 * 60, 7 * 60 + 20};
  c.windows[2] = Window{17 * 60 + 30, 18 * 60 + 30};
  return c;
}

bool parseHHMM(const std::string& s, int& minutes) {
  int h = -1, m = -1, sec = 0, used = 0;
  if (std::sscanf(s.c_str(), "%2d:%2d:%2d%n", &h, &m, &sec, &used) == 3 && size_t(used) == s.size()) {
  } else if (std::sscanf(s.c_str(), "%2d:%2d%n", &h, &m, &used) == 2 && size_t(used) == s.size()) {
    sec = 0;
  } else {
    return false;
  }
  // "5:5" passes sscanf; require two-digit minutes
  size_t colon = s.find(':');
  if (colon == std::string::npos || s.size() < colon + 3) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
  minutes = h * 60 + m;
  return true;
}

static bool readInt(JsonVariantConst v, int& out) {
  if (!v.is<double>()) return false;
  out = int(std::lround(v.as<double>()));
  return true;
}

static std::string fmtHHMM(int minutes) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

bool parseConfig(const std::string& json, Config& inOut, std::string& err) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    err = "bad json";
    return false;
  }
  if (!doc.is<JsonObject>()) {
    err = "not an object";
    return false;
  }
  Config c = inOut;
  if (!doc["mist_seconds"].isNull()) {
    int v;
    if (!readInt(doc["mist_seconds"], v) || v < 5 || v > 120) {
      err = "mist_seconds must be 5-120";
      return false;
    }
    c.mistSeconds = v;
  }
  if (!doc["resume_min_minutes"].isNull()) {
    int v;
    if (!readInt(doc["resume_min_minutes"], v) || v < 0 || v > 90) {
      err = "resume_min_minutes must be 0-90";
      return false;
    }
    c.resumeMinMinutes = v;
  }
  if (!doc["blackouts"].isNull()) {
    if (!doc["blackouts"].is<JsonArray>()) {
      err = "blackouts must be an array";
      return false;
    }
    JsonArrayConst arr = doc["blackouts"].as<JsonArrayConst>();
    if (arr.size() > size_t(kNumWindows)) {
      err = "at most 3 blackouts";
      return false;
    }
    Window w[kNumWindows];
    size_t i = 0;
    for (JsonVariantConst item : arr) {
      int s, e;
      if (!parseHHMM(item["start"] | "", s) || !parseHHMM(item["end"] | "", e)) {
        err = "bad blackout time";
        return false;
      }
      w[i++] = Window{s, e};
    }
    for (int k = 0; k < kNumWindows; ++k) c.windows[k] = w[k];
  }
  inOut = c;
  return true;
}

std::string configToJson(const Config& c) {
  JsonDocument doc;
  doc["mist_seconds"] = c.mistSeconds;
  doc["resume_min_minutes"] = c.resumeMinMinutes;
  JsonArray arr = doc["blackouts"].to<JsonArray>();
  for (const auto& w : c.windows) {
    if (!w.enabled()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["start"] = fmtHHMM(w.startMin);
    o["end"] = fmtHHMM(w.endMin);
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseCommand(const std::string& json, Command& out, std::string& err) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    err = "bad json";
    return false;
  }
  static const struct {
    const char* name;
    CmdType type;
  } kMap[] = {{"mist", CmdType::Mist},           {"pulse", CmdType::Pulse},         {"stop", CmdType::Stop},
              {"refresh", CmdType::Refresh},     {"scheduler", CmdType::Scheduler}, {"test_pump", CmdType::TestPump},
              {"test_led", CmdType::TestLed},    {"test_buzzer", CmdType::TestBuzzer},
              {"test_join", CmdType::TestJoin},  {"reboot", CmdType::Reboot}};
  const char* action = doc["action"] | "";
  Command c;
  for (const auto& m : kMap)
    if (std::strcmp(action, m.name) == 0) c.type = m.type;
  if (c.type == CmdType::Invalid) {
    err = std::string("unknown action: ") + action;
    return false;
  }
  if (c.type == CmdType::Mist && !doc["seconds"].isNull() && !readInt(doc["seconds"], c.seconds)) {
    err = "seconds must be a number";
    return false;
  }
  if (c.type == CmdType::Mist && !doc["force"].isNull()) {
    if (!doc["force"].is<bool>()) {
      err = "force must be true/false";
      return false;
    }
    c.force = doc["force"].as<bool>();
  }
  if (c.type == CmdType::Pulse && !doc["minutes"].isNull() && !readInt(doc["minutes"], c.minutes)) {
    err = "minutes must be a number";
    return false;
  }
  if (c.type == CmdType::Scheduler) {
    if (!doc["on"].is<bool>()) {
      err = "scheduler needs on: true/false";
      return false;
    }
    c.on = doc["on"].as<bool>();
  }
  out = c;
  return true;
}

}  // namespace z
