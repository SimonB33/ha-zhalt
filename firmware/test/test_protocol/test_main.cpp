#include <unity.h>
#include "../fixtures.h"
#include "zprotocol.h"
#include "ztime.h"

using namespace z;

void setUp() {}
void tearDown() {}

// 2026-05-07 08:55:12 Asia/Dubai
static const int64_t kEpochMay7 = 1778129712;

void test_to_local_basic() {
  LocalDateTime t = toLocal(kEpochMay7);
  TEST_ASSERT_EQUAL(2026, t.year);
  TEST_ASSERT_EQUAL(5, t.month);
  TEST_ASSERT_EQUAL(7, t.day);
  TEST_ASSERT_EQUAL(8, t.hour);
  TEST_ASSERT_EQUAL(55, t.minute);
  TEST_ASSERT_EQUAL(12, t.second);
  TEST_ASSERT_EQUAL(4, t.weekday);  // Thursday
}

void test_to_local_leap_day() {
  LocalDateTime t = toLocal(1709236799);  // 2024-02-29 23:59:59 Dubai
  TEST_ASSERT_EQUAL(2024, t.year);
  TEST_ASSERT_EQUAL(2, t.month);
  TEST_ASSERT_EQUAL(29, t.day);
  TEST_ASSERT_EQUAL(23, t.hour);
  TEST_ASSERT_EQUAL(59, t.second);
  TEST_ASSERT_EQUAL(4, t.weekday);
}

void test_minute_of_day() {
  TEST_ASSERT_EQUAL(355, localMinuteOfDay(1792029300));  // 2026-10-15 05:55 Dubai
}

void test_parse_g_imp_header_and_cycles() {
  Settings s;
  TEST_ASSERT_TRUE(parseGImp(kGImp, s));
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL(1, s.machine_type);
  TEST_ASSERT_EQUAL(1, s.progcic_type);
  TEST_ASSERT_EQUAL_STRING("08", s.firmware_minor.c_str());
  const Cycle& c1 = s.cycles[0];
  TEST_ASSERT_EQUAL(1, c1.act);
  TEST_ASSERT_EQUAL(1, c1.mode);
  TEST_ASSERT_EQUAL(5, c1.start_hour);
  TEST_ASSERT_EQUAL(45, c1.start_minute);
  TEST_ASSERT_EQUAL(127, c1.days_bitmap);
  TEST_ASSERT_EQUAL(70, c1.duration_seconds);
  TEST_ASSERT_EQUAL(8, c1.end_hour);
  const Cycle& cs = s.cycles[6];
  TEST_ASSERT_EQUAL(1, cs.act);
  TEST_ASSERT_EQUAL(1, cs.end_hour);
  TEST_ASSERT_EQUAL(30, cs.end_minute);
  TEST_ASSERT_EQUAL(0, s.cycles[4].act);  // C5
  TEST_ASSERT_EQUAL(0, s.cycles[8].act);  // CJ
  TEST_ASSERT_EQUAL_STRING(kGImp, s.raw.c_str());
}

void test_parse_g_imp_rejects_other_frames() {
  Settings s;
  TEST_ASSERT_FALSE(parseGImp("CiaO", s));
  TEST_ASSERT_FALSE(parseGImp("G_dat 1 2 3", s));
}

void test_parse_g_imp_truncated_is_not_valid() {
  Settings s;
  TEST_ASSERT_TRUE(parseGImp("G_imp 1 0 1 2 08 A C1 1 1 5 45 127 70 8 0 10 5", s));
  TEST_ASSERT_FALSE(s.valid);
}

void test_parse_g_dat_states() {
  Dat d;
  TEST_ASSERT_TRUE(parseGDat(kGDatStandby, d));
  TEST_ASSERT_EQUAL(7, d.op_mode);
  TEST_ASSERT_EQUAL(0, d.cycle_state);
  TEST_ASSERT_EQUAL(1, d.active_cycle);
  TEST_ASSERT_EQUAL(2026, d.dev_year);
  TEST_ASSERT_EQUAL(5, d.dev_month);
  TEST_ASSERT_EQUAL(7, d.dev_day);
  TEST_ASSERT_FALSE(d.spraying());

  TEST_ASSERT_TRUE(parseGDat(kGDatMisting, d));
  TEST_ASSERT_EQUAL(9, d.op_mode);
  TEST_ASSERT_EQUAL(12, d.active_cycle);
  TEST_ASSERT_EQUAL(70, d.planned);
  TEST_ASSERT_EQUAL(200, d.elapsed);
  TEST_ASSERT_TRUE(d.spraying());

  // op 6 with cycle_state 2: phase-2 spray can still be running, so it counts as spraying
  TEST_ASSERT_TRUE(parseGDat(kGDatJustStopped, d));
  TEST_ASSERT_EQUAL(6, d.op_mode);
  TEST_ASSERT_EQUAL(2, d.cycle_state);
  TEST_ASSERT_TRUE(d.spraying());
}

void test_parse_g_dat_rejects_short_or_other() {
  Dat d;
  TEST_ASSERT_FALSE(parseGDat("G_dat 1 2 3", d));
  TEST_ASSERT_FALSE(parseGDat("G_imp 1", d));
}

void test_build_p_dat() {
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::None).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;1;0;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Mist).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;1;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Pulse).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;1;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Stop).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;0;0;1;0;0;0;0;0;1;0;0;", buildPDat(Action::TestPump).c_str());
}

void test_build_handshake() {
  TEST_ASSERT_EQUAL_STRING("P_imp;4;7;5;2026;8;55;12;en-US;2113;A;0;",
                           buildPImpHandshake(toLocal(kEpochMay7)).c_str());
}

void test_build_settings_round_trip() {
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_EQUAL_STRING(
      "P_imp;4;7;5;2026;8;55;12;en-US;2113;B;1;C1 1;1;5;45;127;70;8;0;10;5;C2 1;2;6;0;127;70;7;0;10;5;"
      "C3 1;1;18;30;127;70;20;0;10;5;C4 1;2;19;0;127;70;20;30;10;5;C5 0;1;19;0;127;70;20;30;10;5;"
      "C6 0;1;21;0;127;70;22;30;10;5;CS 1;1;127;70;1;30;10;5;CE 0;1;127;70;1;30;10;5;"
      "CJ 0;1;127;70;1;30;10;5;0;1001000;0;1;0;freezanz;0;0;0;1;",
      buildPImpSettings(s, toLocal(kEpochMay7)).c_str());
}

void test_build_settings_all_off() {
  Settings s;
  parseGImp(kGImp, s);
  Settings off = withAllCyclesActive(s, false);
  TEST_ASSERT_EQUAL_STRING(
      "P_imp;4;7;5;2026;8;55;12;en-US;2113;B;1;C1 0;1;5;45;127;70;8;0;10;5;C2 0;2;6;0;127;70;7;0;10;5;"
      "C3 0;1;18;30;127;70;20;0;10;5;C4 0;2;19;0;127;70;20;30;10;5;C5 0;1;19;0;127;70;20;30;10;5;"
      "C6 0;1;21;0;127;70;22;30;10;5;CS 0;1;127;70;1;30;10;5;CE 0;1;127;70;1;30;10;5;"
      "CJ 0;1;127;70;1;30;10;5;0;0;0;1;0;freezanz;0;0;0;1;",
      buildPImpSettings(off, toLocal(kEpochMay7)).c_str());
  TEST_ASSERT_EQUAL(1, s.cycles[0].act);  // original untouched
}

void test_cicliplus_and_any_active() {
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_EQUAL(1001000L, cicliplus(s));
  TEST_ASSERT_TRUE(anyCycleActive(s));
  Settings off;
  parseGImp(kGImpAllOff, off);
  TEST_ASSERT_EQUAL(0L, cicliplus(off));
  TEST_ASSERT_FALSE(anyCycleActive(off));
}

void test_names() {
  TEST_ASSERT_EQUAL_STRING("Misting", opModeName(9).c_str());
  TEST_ASSERT_EQUAL_STRING("Standby", opModeName(7).c_str());
  TEST_ASSERT_EQUAL_STRING("Stopped", opModeName(6).c_str());
  TEST_ASSERT_EQUAL_STRING("Unknown(3)", opModeName(3).c_str());
  TEST_ASSERT_EQUAL_STRING("Manual", activeCycleName(12).c_str());
  TEST_ASSERT_EQUAL_STRING("None", activeCycleName(0).c_str());
  TEST_ASSERT_EQUAL_STRING("Cycle1", activeCycleName(1).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_to_local_basic);
  RUN_TEST(test_to_local_leap_day);
  RUN_TEST(test_minute_of_day);
  RUN_TEST(test_parse_g_imp_header_and_cycles);
  RUN_TEST(test_parse_g_imp_rejects_other_frames);
  RUN_TEST(test_parse_g_imp_truncated_is_not_valid);
  RUN_TEST(test_parse_g_dat_states);
  RUN_TEST(test_parse_g_dat_rejects_short_or_other);
  RUN_TEST(test_build_p_dat);
  RUN_TEST(test_build_handshake);
  RUN_TEST(test_build_settings_round_trip);
  RUN_TEST(test_build_settings_all_off);
  RUN_TEST(test_cicliplus_and_any_active);
  RUN_TEST(test_names);
  return UNITY_END();
}
