#include "zpublish.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "zprotocol.h"
#include "ztime.h"

namespace z {

std::string isoLocal(int64_t epoch) {
  if (epoch <= 0) return "";
  LocalDateTime t = toLocal(epoch);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d+04:00", t.year, t.month, t.day, t.hour, t.minute,
                t.second);
  return buf;
}

std::string deriveState(const SafetyState& s, bool mistActive) {
  if (s.stopFailed) return "stop_failed";
  if (s.stopPending) return "stopping";
  if (mistActive) return "misting";
  if (s.pulseActive) return "pulse_session";
  if (s.paused) return "paused_blackout";
  return "idle";
}

static void putTime(JsonDocument& d, const char* key, int64_t epoch) {
  if (epoch > 0)
    d[key] = isoLocal(epoch);
  else
    d[key] = nullptr;
}

static void putStr(JsonDocument& d, const char* key, const std::string& v) {
  if (v.empty())
    d[key] = nullptr;
  else
    d[key] = v;
}

std::string stateJson(const BridgeSnapshot& s) {
  JsonDocument d;
  d["state"] = s.state;
  d["misting"] = s.misting;
  putTime(d, "session_end", s.sessionEnd);
  putTime(d, "last_mist", s.lastMist);
  putStr(d, "last_mist_kind", s.lastMistKind);
  putStr(d, "last_result", s.lastResult);
  if (s.schedulerOn < 0)
    d["scheduler_on"] = nullptr;
  else
    d["scheduler_on"] = s.schedulerOn == 1;
  if (s.unitValid) {
    d["unit_state"] = opModeName(s.unitOp);
    d["active_cycle"] = activeCycleName(s.unitActiveCycle);
  } else {
    d["unit_state"] = nullptr;
    d["active_cycle"] = nullptr;
  }
  putStr(d, "unit_clock", s.unitClock);
  putStr(d, "unit_firmware", s.unitFirmware);
  putTime(d, "last_contact", s.lastContact);
  d["zhalt_reachable"] = s.zhaltReachable;
  if (s.rssiKnown)
    d["rssi"] = s.rssi;
  else
    d["rssi"] = nullptr;
  if (s.joinMs > 0)
    d["join_ms"] = s.joinMs;
  else
    d["join_ms"] = nullptr;
  d["join_failures_24h"] = s.joinFailures24h;
  d["uptime_s"] = s.uptimeS;
  d["free_heap"] = s.freeHeap;
  putStr(d, "reset_reason", s.resetReason);
  putStr(d, "fw", s.fw);
  putStr(d, "ip", s.ip);
  d["passthrough"] = s.passthrough;
  std::string out;
  serializeJson(d, out);
  return out;
}

namespace {

struct Def {
  const char* component;
  const char* key;
  const char* name;
  bool bridge;
  const char* tmpl;
  const char* press;
  const char* devClass;
  const char* unit;
  const char* icon;
  bool diag;
};

const Def kDefs[] = {
    // Zhalt Mosquito (unit)
    {"button", "mist_now", "Mist now", false, nullptr, R"({"action":"mist"})", nullptr, nullptr, "mdi:weather-fog", false},
    {"button", "pulse_now", "Start pulse session", false, nullptr, R"({"action":"pulse"})", nullptr, nullptr, "mdi:timer-play", false},
    {"button", "stop", "Stop", false, nullptr, R"({"action":"stop"})", nullptr, nullptr, "mdi:stop-circle", false},
    {"button", "refresh", "Refresh", false, nullptr, R"({"action":"refresh"})", nullptr, nullptr, "mdi:refresh", true},
    {"button", "test_pump", "Test pump", false, nullptr, R"({"action":"test_pump"})", nullptr, nullptr, "mdi:pump", true},
    {"button", "test_led", "Test LED", false, nullptr, R"({"action":"test_led"})", nullptr, nullptr, "mdi:led-on", true},
    {"button", "test_buzzer", "Test buzzer", false, nullptr, R"({"action":"test_buzzer"})", nullptr, nullptr, "mdi:volume-high", true},
    {"sensor", "state", "State", false, "{{ value_json.state }}", nullptr, nullptr, nullptr, "mdi:state-machine", false},
    {"sensor", "session_ends_at", "Session ends at", false, "{{ value_json.session_end }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"sensor", "last_mist", "Last mist", false, "{{ value_json.last_mist }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"sensor", "last_result", "Last result", false, "{{ value_json.last_result }}", nullptr, nullptr, nullptr, "mdi:clipboard-check", false},
    {"sensor", "unit_state", "Unit state", false, "{{ value_json.unit_state }}", nullptr, nullptr, nullptr, "mdi:spray", false},
    {"sensor", "active_cycle", "Active cycle", false, "{{ value_json.active_cycle }}", nullptr, nullptr, nullptr, "mdi:repeat", false},
    {"sensor", "unit_clock", "Unit clock", false, "{{ value_json.unit_clock }}", nullptr, nullptr, nullptr, "mdi:clock-outline", true},
    {"sensor", "unit_firmware", "Unit firmware", false, "{{ value_json.unit_firmware }}", nullptr, nullptr, nullptr, "mdi:chip", true},
    {"sensor", "last_contact", "Last contact with unit", false, "{{ value_json.last_contact }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"binary_sensor", "misting", "Misting", false, "{{ 'ON' if value_json.misting else 'OFF' }}", nullptr, "running", nullptr, nullptr, false},
    // Zhalt Bridge
    {"binary_sensor", "zhalt_reachable", "Zhalt reachable", true, "{{ 'ON' if value_json.zhalt_reachable else 'OFF' }}", nullptr, "connectivity", nullptr, nullptr, true},
    {"binary_sensor", "passthrough_active", "Pass-through active", true, "{{ 'ON' if value_json.passthrough else 'OFF' }}", nullptr, "running", nullptr, nullptr, true},
    {"sensor", "wifi_signal", "Wi-Fi signal", true, "{{ value_json.rssi }}", nullptr, "signal_strength", "dBm", nullptr, true},
    {"sensor", "join_time", "Join time", true, "{{ value_json.join_ms }}", nullptr, "duration", "ms", nullptr, true},
    {"sensor", "join_failures_24h", "Join failures (24 h)", true, "{{ value_json.join_failures_24h }}", nullptr, nullptr, nullptr, "mdi:wifi-alert", true},
    {"sensor", "uptime", "Uptime", true, "{{ value_json.uptime_s }}", nullptr, "duration", "s", nullptr, true},
    {"sensor", "free_heap", "Free memory", true, "{{ value_json.free_heap }}", nullptr, "data_size", "B", nullptr, true},
    {"sensor", "reset_reason", "Reboot reason", true, "{{ value_json.reset_reason }}", nullptr, nullptr, nullptr, "mdi:restart-alert", true},
    {"sensor", "firmware", "Firmware", true, "{{ value_json.fw }}", nullptr, nullptr, nullptr, "mdi:chip", true},
    {"button", "reboot", "Reboot", true, nullptr, R"({"action":"reboot"})", "restart", nullptr, nullptr, true},
    {"button", "test_join", "Test join", true, nullptr, R"({"action":"test_join"})", nullptr, nullptr, "mdi:wifi-sync", true},
};

}  // namespace

std::vector<DiscoveryMsg> discoveryMessages(const std::string& mac, const std::string& fw, const std::string& ip) {
  std::vector<DiscoveryMsg> out;
  const std::string bridgeId = "zhalt_bridge_" + mac;

  auto base = [&](JsonDocument& doc, const char* component, const char* key, const char* name, bool bridge) {
    doc["name"] = name;
    doc["unique_id"] = "zhalt_" + mac + "_" + key;
    doc["default_entity_id"] = std::string(component) + ".zhalt_mqtt_" + key;
    JsonObject dev = doc["device"].to<JsonObject>();
    if (bridge) {
      dev["identifiers"].to<JsonArray>().add(bridgeId);
      dev["name"] = "Zhalt Bridge";
      dev["manufacturer"] = "Waveshare";
      dev["model"] = "ESP32-S3-ETH";
      dev["sw_version"] = fw;
      dev["configuration_url"] = "http://" + ip + "/";
    } else {
      dev["identifiers"].to<JsonArray>().add("zhalt_unit_" + mac);
      dev["name"] = "Zhalt Mosquito";
      dev["manufacturer"] = "Zhalt";
      dev["model"] = "Evolution Connect";
      dev["via_device"] = bridgeId;
    }
  };
  auto emit = [&](const char* component, const char* key, JsonDocument& doc) {
    DiscoveryMsg m;
    m.topic = std::string("homeassistant/") + component + "/zhalt_" + mac + "/" + key + "/config";
    serializeJson(doc, m.payload);
    out.push_back(m);
  };

  // Bridge device first, so the unit's via_device exists when HA reads the unit entities.
  {
    JsonDocument doc;
    base(doc, "binary_sensor", "online", "Bridge online", true);
    doc["state_topic"] = kTopicStatus;
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    doc["device_class"] = "connectivity";
    emit("binary_sensor", "online", doc);
  }

  for (const bool bridgePass : {true, false}) {
    for (const Def& d : kDefs) {
      if (d.bridge != bridgePass) continue;
      JsonDocument doc;
      base(doc, d.component, d.key, d.name, d.bridge);
      doc["availability_topic"] = kTopicStatus;
      if (d.press) {
        doc["command_topic"] = kTopicCmd;
        doc["payload_press"] = d.press;
      }
      if (d.tmpl) {
        doc["state_topic"] = kTopicState;
        doc["value_template"] = d.tmpl;
      }
      if (d.devClass) doc["device_class"] = d.devClass;
      if (d.unit) doc["unit_of_measurement"] = d.unit;
      if (d.icon) doc["icon"] = d.icon;
      if (d.diag) doc["entity_category"] = "diagnostic";
      if (std::strcmp(d.key, "last_mist") == 0) {
        doc["json_attributes_topic"] = kTopicState;
        doc["json_attributes_template"] = "{{ {'kind': value_json.last_mist_kind} | tojson }}";
      }
      emit(d.component, d.key, doc);
    }
  }

  {
    JsonDocument doc;
    base(doc, "switch", "onboard_scheduler", "Onboard scheduler", false);
    doc["availability_topic"] = kTopicStatus;
    doc["command_topic"] = kTopicCmd;
    doc["payload_on"] = R"({"action":"scheduler","on":true})";
    doc["payload_off"] = R"({"action":"scheduler","on":false})";
    doc["state_on"] = "ON";
    doc["state_off"] = "OFF";
    doc["state_topic"] = kTopicState;
    doc["value_template"] =
        "{% if value_json.scheduler_on is none %}None{% elif value_json.scheduler_on %}ON{% else %}OFF{% endif %}";
    doc["icon"] = "mdi:calendar-clock";
    emit("switch", "onboard_scheduler", doc);
  }
  return out;
}

}  // namespace z
