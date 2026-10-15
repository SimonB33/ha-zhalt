#include <ArduinoJson.h>
#include <unity.h>

#include <set>

#include "zpublish.h"

using namespace z;

void setUp() {}
void tearDown() {}

void test_iso_local() {
  TEST_ASSERT_EQUAL_STRING("2026-10-15T05:55:00+04:00", isoLocal(1792029300).c_str());
  TEST_ASSERT_EQUAL_STRING("", isoLocal(0).c_str());
}

void test_derive_state() {
  SafetyState s;
  TEST_ASSERT_EQUAL_STRING("idle", deriveState(s, false).c_str());
  TEST_ASSERT_EQUAL_STRING("misting", deriveState(s, true).c_str());
  s.pulseActive = true;
  TEST_ASSERT_EQUAL_STRING("pulse_session", deriveState(s, false).c_str());
  s.pulseActive = false;
  s.paused = true;
  TEST_ASSERT_EQUAL_STRING("paused_blackout", deriveState(s, false).c_str());
  s.stopPending = true;
  TEST_ASSERT_EQUAL_STRING("stopping", deriveState(s, false).c_str());
  s.stopFailed = true;
  TEST_ASSERT_EQUAL_STRING("stop_failed", deriveState(s, false).c_str());
}

void test_state_json_idle_has_nulls() {
  BridgeSnapshot s;
  JsonDocument d;
  deserializeJson(d, stateJson(s));
  TEST_ASSERT_EQUAL_STRING("idle", d["state"].as<const char*>());
  TEST_ASSERT_TRUE(d["session_end"].isNull());
  TEST_ASSERT_TRUE(d["last_mist"].isNull());
  TEST_ASSERT_TRUE(d["scheduler_on"].isNull());
  TEST_ASSERT_TRUE(d["unit_state"].isNull());
  TEST_ASSERT_TRUE(d["rssi"].isNull());
  TEST_ASSERT_FALSE(d["misting"].as<bool>());
}

void test_state_json_values() {
  BridgeSnapshot s;
  s.state = "pulse_session";
  s.misting = true;
  s.sessionEnd = 1792029300;
  s.lastMist = 1792029300;
  s.lastMistKind = "pulse";
  s.lastResult = "ok";
  s.schedulerOn = 0;
  s.unitValid = true;
  s.unitOp = 9;
  s.unitActiveCycle = 12;
  s.rssiKnown = true;
  s.rssi = -61;
  s.joinMs = 3200;
  JsonDocument d;
  deserializeJson(d, stateJson(s));
  TEST_ASSERT_EQUAL_STRING("2026-10-15T05:55:00+04:00", d["session_end"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("pulse", d["last_mist_kind"].as<const char*>());
  TEST_ASSERT_FALSE(d["scheduler_on"].as<bool>());
  TEST_ASSERT_FALSE(d["scheduler_on"].isNull());
  TEST_ASSERT_EQUAL_STRING("Misting", d["unit_state"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Manual", d["active_cycle"].as<const char*>());
  TEST_ASSERT_EQUAL(-61, d["rssi"].as<int>());
  TEST_ASSERT_EQUAL(3200, d["join_ms"].as<int>());
}

static JsonDocument findPayload(const std::vector<DiscoveryMsg>& msgs, const std::string& topic) {
  JsonDocument d;
  for (const auto& m : msgs)
    if (m.topic == topic) deserializeJson(d, m.payload);
  return d;
}

void test_discovery_count_and_unique_ids() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  TEST_ASSERT_EQUAL(30, (int)msgs.size());
  std::set<std::string> ids, topics;
  for (const auto& m : msgs) {
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, m.payload));
    ids.insert(d["unique_id"].as<std::string>());
    topics.insert(m.topic);
  }
  TEST_ASSERT_EQUAL(30, (int)ids.size());
  TEST_ASSERT_EQUAL(30, (int)topics.size());
}

void test_discovery_mist_button() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/button/zhalt_aabbcc/mist_now/config");
  TEST_ASSERT_EQUAL_STRING("button.zhalt_mqtt_mist_now", d["default_entity_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt/cmd", d["command_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"mist\"}", d["payload_press"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt/bridge/status", d["availability_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Zhalt Mosquito", d["device"]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt_bridge_aabbcc", d["device"]["via_device"].as<const char*>());
}

void test_discovery_online_sensor_has_no_availability() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/binary_sensor/zhalt_aabbcc/online/config");
  TEST_ASSERT_TRUE(d["availability_topic"].isNull());
  TEST_ASSERT_EQUAL_STRING("zhalt/bridge/status", d["state_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("online", d["payload_on"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Zhalt Bridge", d["device"]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.30/", d["device"]["configuration_url"].as<const char*>());
}

void test_discovery_scheduler_switch() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/switch/zhalt_aabbcc/onboard_scheduler/config");
  TEST_ASSERT_EQUAL_STRING("switch.zhalt_mqtt_onboard_scheduler", d["default_entity_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"scheduler\",\"on\":false}", d["payload_off"].as<const char*>());
}

void test_discovery_bridge_device_first() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  int firstUnit = -1;
  for (int i = 0; i < (int)msgs.size(); i++) {
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, msgs[i].payload));
    if (firstUnit < 0 && std::string(d["device"]["name"].as<const char*>()) == "Zhalt Mosquito") firstUnit = i;
  }
  TEST_ASSERT_EQUAL(12, firstUnit);
  for (int i = 0; i < (int)msgs.size(); i++) {
    JsonDocument d;
    deserializeJson(d, msgs[i].payload);
    const char* name = d["device"]["name"].as<const char*>();
    if (i < firstUnit)
      TEST_ASSERT_EQUAL_STRING("Zhalt Bridge", name);
    else
      TEST_ASSERT_EQUAL_STRING("Zhalt Mosquito", name);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_iso_local);
  RUN_TEST(test_derive_state);
  RUN_TEST(test_state_json_idle_has_nulls);
  RUN_TEST(test_state_json_values);
  RUN_TEST(test_discovery_count_and_unique_ids);
  RUN_TEST(test_discovery_mist_button);
  RUN_TEST(test_discovery_online_sensor_has_no_availability);
  RUN_TEST(test_discovery_scheduler_switch);
  RUN_TEST(test_discovery_bridge_device_first);
  return UNITY_END();
}
