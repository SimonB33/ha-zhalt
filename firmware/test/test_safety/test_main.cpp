#include <unity.h>
#include "zsafety.h"

using namespace z;

void setUp() {}
void tearDown() {}

// Epoch for 2026-10-15 hh:mm Dubai (05:55 = 1792029300)
static int64_t at(int h, int m) { return 1792029300LL + int64_t((h * 60 + m) - 355) * 60; }

void test_clamps() {
  Safety s;
  TEST_ASSERT_EQUAL(70, s.clampMistSeconds(0));
  TEST_ASSERT_EQUAL(15, s.clampMistSeconds(15));
  TEST_ASSERT_EQUAL(120, s.clampMistSeconds(200));
  TEST_ASSERT_EQUAL(40, s.clampPulseMinutes(0));
  TEST_ASSERT_EQUAL(10, s.clampPulseMinutes(5));
  TEST_ASSERT_EQUAL(90, s.clampPulseMinutes(200));
}

void test_gate_clock_and_blackout() {
  Safety s;
  TEST_ASSERT_TRUE(s.canStart(at(5, 50), false) == Gate::ClockNotSet);
  TEST_ASSERT_TRUE(s.canStart(at(5, 50), true) == Gate::Ok);
  TEST_ASSERT_TRUE(s.canStart(at(6, 0), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(6, 15), true) == Gate::Ok);
  TEST_ASSERT_TRUE(s.canStart(at(7, 10), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(17, 30), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(18, 30), true) == Gate::Ok);
}

void test_gate_pulse_active() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.canStart(at(8, 10), true) == Gate::PulseActive);
}

void test_pulse_cap_stops_once() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.tick(at(8, 39), true).action == TickAction::None);
  TEST_ASSERT_TRUE(s.tick(at(8, 40), true).action == TickAction::Stop);
  TEST_ASSERT_TRUE(s.tick(at(8, 40) + 1, true).action == TickAction::None);  // in flight
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(8, 41), true).action == TickAction::None);
  TEST_ASSERT_FALSE(s.state().pulseActive);
  TEST_ASSERT_FALSE(s.state().stopPending);
}

void test_blackout_stops_then_resumes() {
  Safety s;
  s.onPulseStarted(at(5, 45), 40);  // would end 06:25
  TEST_ASSERT_TRUE(s.tick(at(5, 54), true).action == TickAction::None);
  TEST_ASSERT_TRUE(s.tick(at(5, 55), true).action == TickAction::Stop);
  TEST_ASSERT_TRUE(s.state().paused);
  TEST_ASSERT_EQUAL(30 * 60, (int)s.state().pausedRemainingSec);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(6, 10), true).action == TickAction::None);
  TickResult r = s.tick(at(6, 15), true);
  TEST_ASSERT_TRUE(r.action == TickAction::ResumePulse);
  TEST_ASSERT_EQUAL(30, r.minutes);
  TEST_ASSERT_TRUE(s.tick(at(6, 16), true).action == TickAction::None);
}

void test_blackout_no_resume_below_threshold() {
  Safety s;
  s.onPulseStarted(at(5, 50), 10);  // would end 06:00, 5 min left at 05:55
  TEST_ASSERT_TRUE(s.tick(at(5, 55), true).action == TickAction::Stop);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(6, 15), true).action == TickAction::None);
  TEST_ASSERT_FALSE(s.state().paused);
}

void test_blackout_resume_threshold_zero_always_resumes() {
  Safety s;
  Config c = defaultConfig();
  c.resumeMinMinutes = 0;
  s.setConfig(c);
  s.onPulseStarted(at(5, 50), 10);
  s.tick(at(5, 55), true);
  s.onStopConfirmed();
  TickResult r = s.tick(at(6, 15), true);
  TEST_ASSERT_TRUE(r.action == TickAction::ResumePulse);
  TEST_ASSERT_EQUAL(5, r.minutes);
}

void test_stop_retries_then_fails() {
  Safety s;
  int64_t t = at(9, 0);
  s.requestStop(t);
  TEST_ASSERT_TRUE(s.tick(t, true).action == TickAction::Stop);
  for (int retry = 1; retry <= 3; ++retry) {
    s.onStopFailed(t);
    TEST_ASSERT_TRUE(s.tick(t + 19, true).action == TickAction::None);
    t += 20;
    TEST_ASSERT_TRUE(s.tick(t, true).action == TickAction::Stop);
  }
  s.onStopFailed(t);
  TEST_ASSERT_TRUE(s.state().stopFailed);
  TEST_ASSERT_FALSE(s.state().stopPending);
  TEST_ASSERT_TRUE(s.tick(t + 100, true).action == TickAction::None);
}

void test_user_stop_cancels_pulse() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  s.requestStop(at(8, 5));
  TEST_ASSERT_TRUE(s.tick(at(8, 5), true).action == TickAction::Stop);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(8, 40), true).action == TickAction::None);
}

void test_clock_invalid_defers_timers_but_not_stops() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.tick(at(8, 41), false).action == TickAction::None);
  s.requestStop(at(8, 41));
  TEST_ASSERT_TRUE(s.tick(at(8, 41), false).action == TickAction::Stop);
}

void test_restore_after_reboot_still_stops() {
  Safety before;
  before.onPulseStarted(at(8, 0), 40);
  std::string saved = before.serialize();

  SafetyState st;
  TEST_ASSERT_TRUE(Safety::deserialize(saved, st));
  Safety after;
  after.restore(st);
  TEST_ASSERT_TRUE(after.state().pulseActive);
  TEST_ASSERT_TRUE(after.tick(at(8, 40), true).action == TickAction::Stop);
}

void test_restore_with_stop_in_flight_retries() {
  Safety before;
  before.requestStop(at(9, 0));
  before.tick(at(9, 0), true);  // stop in flight, then reboot
  SafetyState st;
  TEST_ASSERT_TRUE(Safety::deserialize(before.serialize(), st));
  Safety after;
  after.restore(st);
  TEST_ASSERT_TRUE(after.tick(at(9, 1), true).action == TickAction::Stop);
}

void test_deserialize_rejects_garbage() {
  SafetyState st;
  TEST_ASSERT_FALSE(Safety::deserialize("", st));
  TEST_ASSERT_FALSE(Safety::deserialize("nope", st));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clamps);
  RUN_TEST(test_gate_clock_and_blackout);
  RUN_TEST(test_gate_pulse_active);
  RUN_TEST(test_pulse_cap_stops_once);
  RUN_TEST(test_blackout_stops_then_resumes);
  RUN_TEST(test_blackout_no_resume_below_threshold);
  RUN_TEST(test_blackout_resume_threshold_zero_always_resumes);
  RUN_TEST(test_stop_retries_then_fails);
  RUN_TEST(test_user_stop_cancels_pulse);
  RUN_TEST(test_clock_invalid_defers_timers_but_not_stops);
  RUN_TEST(test_restore_after_reboot_still_stops);
  RUN_TEST(test_restore_with_stop_in_flight_retries);
  RUN_TEST(test_deserialize_rejects_garbage);
  return UNITY_END();
}
