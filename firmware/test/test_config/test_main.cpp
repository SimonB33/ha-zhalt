#include <unity.h>
#include "zconfig.h"

using namespace z;

void setUp() {}
void tearDown() {}

void test_defaults() {
  Config c = defaultConfig();
  TEST_ASSERT_EQUAL(70, c.mistSeconds);
  TEST_ASSERT_EQUAL(10, c.resumeMinMinutes);
  TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
  TEST_ASSERT_EQUAL(375, c.windows[0].endMin);
  TEST_ASSERT_EQUAL(420, c.windows[1].startMin);
  TEST_ASSERT_EQUAL(440, c.windows[1].endMin);
  TEST_ASSERT_EQUAL(1050, c.windows[2].startMin);
  TEST_ASSERT_EQUAL(1110, c.windows[2].endMin);
}

void test_window_contains() {
  Window w{355, 375};
  TEST_ASSERT_FALSE(w.contains(354));
  TEST_ASSERT_TRUE(w.contains(355));
  TEST_ASSERT_TRUE(w.contains(374));
  TEST_ASSERT_FALSE(w.contains(375));
  Window night{1380, 60};  // 23:00-01:00
  TEST_ASSERT_TRUE(night.contains(1439));
  TEST_ASSERT_TRUE(night.contains(0));
  TEST_ASSERT_FALSE(night.contains(60));
  Window off;
  TEST_ASSERT_FALSE(off.enabled());
  TEST_ASSERT_FALSE(off.contains(0));
}

void test_parse_hhmm() {
  int m;
  TEST_ASSERT_TRUE(parseHHMM("05:55", m));
  TEST_ASSERT_EQUAL(355, m);
  TEST_ASSERT_TRUE(parseHHMM("05:55:00", m));
  TEST_ASSERT_EQUAL(355, m);
  TEST_ASSERT_TRUE(parseHHMM("7:00", m));
  TEST_ASSERT_EQUAL(420, m);
  TEST_ASSERT_FALSE(parseHHMM("25:00", m));
  TEST_ASSERT_FALSE(parseHHMM("05:60", m));
  TEST_ASSERT_FALSE(parseHHMM("05:55x", m));
  TEST_ASSERT_FALSE(parseHHMM("", m));
}

void test_parse_config_full() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(
      R"({"mist_seconds":60,"resume_min_minutes":15,"blackouts":[{"start":"05:50:00","end":"06:10:00"}]})", c, err));
  TEST_ASSERT_EQUAL(60, c.mistSeconds);
  TEST_ASSERT_EQUAL(15, c.resumeMinMinutes);
  TEST_ASSERT_EQUAL(350, c.windows[0].startMin);
  TEST_ASSERT_EQUAL(370, c.windows[0].endMin);
  TEST_ASSERT_FALSE(c.windows[1].enabled());  // a shorter list disables the rest
  TEST_ASSERT_FALSE(c.windows[2].enabled());
}

void test_parse_config_accepts_floats() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(R"({"mist_seconds":70.0,"resume_min_minutes":10.0})", c, err));
  TEST_ASSERT_EQUAL(70, c.mistSeconds);
}

void test_parse_config_partial_keeps_rest() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(R"({"mist_seconds":45})", c, err));
  TEST_ASSERT_EQUAL(45, c.mistSeconds);
  TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
}

void test_parse_config_rejects_bad_input_and_keeps_previous() {
  const char* bad[] = {
      "not json",
      R"({"mist_seconds":200})",
      R"({"mist_seconds":"seventy"})",
      R"({"resume_min_minutes":-1})",
      R"({"blackouts":[{"start":"25:00","end":"06:00"}]})",
      R"({"blackouts":[{"start":"5:5","end":"06:00"}]})",
      R"({"blackouts":[{"start":"01:00","end":"02:00"},{"start":"03:00","end":"04:00"},{"start":"05:00","end":"06:00"},{"start":"07:00","end":"08:00"}]})",
      R"({"blackouts":"05:55-06:15"})",
  };
  for (const char* json : bad) {
    Config c = defaultConfig();
    std::string err;
    TEST_ASSERT_FALSE_MESSAGE(parseConfig(json, c, err), json);
    TEST_ASSERT_FALSE(err.empty());
    TEST_ASSERT_EQUAL(70, c.mistSeconds);
    TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
  }
}

void test_config_json_round_trip() {
  Config c = defaultConfig();
  c.mistSeconds = 55;
  Config back;
  back.mistSeconds = 1;
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(configToJson(c), back, err));
  TEST_ASSERT_EQUAL(55, back.mistSeconds);
  TEST_ASSERT_EQUAL(1050, back.windows[2].startMin);
  TEST_ASSERT_EQUAL(1110, back.windows[2].endMin);
}

void test_parse_commands() {
  Command c;
  std::string err;
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"mist","seconds":70})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Mist);
  TEST_ASSERT_EQUAL(70, c.seconds);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"mist"})", c, err));
  TEST_ASSERT_EQUAL(0, c.seconds);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"pulse","minutes":40})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Pulse);
  TEST_ASSERT_EQUAL(40, c.minutes);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"scheduler","on":false})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Scheduler);
  TEST_ASSERT_FALSE(c.on);
  const char* simple[] = {"stop", "refresh", "test_pump", "test_led", "test_buzzer", "test_join", "reboot"};
  CmdType types[] = {CmdType::Stop, CmdType::Refresh, CmdType::TestPump, CmdType::TestLed,
                     CmdType::TestBuzzer, CmdType::TestJoin, CmdType::Reboot};
  for (int i = 0; i < 7; ++i) {
    std::string json = std::string(R"({"action":")") + simple[i] + "\"}";
    TEST_ASSERT_TRUE(parseCommand(json, c, err));
    TEST_ASSERT_TRUE(c.type == types[i]);
  }
}

void test_parse_commands_rejects() {
  Command c;
  std::string err;
  TEST_ASSERT_FALSE(parseCommand("", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"explode"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"scheduler"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"mist","seconds":"x"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"(["mist"])", c, err));
}

void test_stop_ignores_stray_fields() {
  Command c;
  std::string err;
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"stop","seconds":"x"})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Stop);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"stop","minutes":[1]})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Stop);
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"mist","seconds":"x"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"pulse","minutes":"x"})", c, err));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_window_contains);
  RUN_TEST(test_parse_hhmm);
  RUN_TEST(test_parse_config_full);
  RUN_TEST(test_parse_config_accepts_floats);
  RUN_TEST(test_parse_config_partial_keeps_rest);
  RUN_TEST(test_parse_config_rejects_bad_input_and_keeps_previous);
  RUN_TEST(test_config_json_round_trip);
  RUN_TEST(test_parse_commands);
  RUN_TEST(test_parse_commands_rejects);
  RUN_TEST(test_stop_ignores_stray_fields);
  return UNITY_END();
}
