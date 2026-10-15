#include <unity.h>
#include "../fixtures.h"
#include "zconversation.h"

using namespace z;
using St = Conversation::Status;

void setUp() {}
void tearDown() {}

static const LocalDateTime kClock = toLocal(1778129712);  // 2026-05-07 08:55:12 Dubai
static const std::string kHandshake = "P_imp;4;7;5;2026;8;55;12;en-US;2113;A;0;";
static const std::string kPollNone = "P_dat;1920;1080;0;0;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollMist = "P_dat;1920;1080;1;0;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollPulse = "P_dat;1920;1080;0;1;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollStop = "P_dat;1920;1080;0;0;1;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollPump = "P_dat;1920;1080;0;0;0;0;1;0;0;0;0;0;1;0;0;";

static bool has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

// Connect at t=0, greeting at t=10, G_imp at t=110 (enters the act phase at 110).
static std::vector<std::string> open(Conversation& c) {
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onFrame(kGImp, 110, kClock);
  return c.takeOutgoing();
}

void test_refresh_happy_path() {
  Conversation c(Goal::Refresh);
  auto out = open(c);
  TEST_ASSERT_EQUAL_STRING(kHandshake.c_str(), out.at(0).c_str());
  TEST_ASSERT_TRUE(has(out, kPollNone));
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 300, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_TRUE(c.settings().valid);
  TEST_ASSERT_EQUAL(7, c.lastDat().op_mode);
}

void test_greeting_timeout() {
  Conversation c(Goal::Refresh);
  c.onConnected(0);
  c.onTick(7999, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onTick(8000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("no_greeting", c.error().c_str());
}

void test_no_g_imp_still_acts_after_wait() {
  Conversation c(Goal::Refresh);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onTick(1510, kClock);
  auto ka = c.takeOutgoing();
  TEST_ASSERT_TRUE(has(ka, kPollNone));  // keepalive while waiting for G_imp
  c.onTick(4010, kClock);
  c.onFrame(kGDatStandby, 4200, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_FALSE(c.settings().valid);
}

void test_mist_timed_stop() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(15000);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollMist));
  c.onFrame(kGDatMisting, 2000, kClock);
  TEST_ASSERT_TRUE(c.started());
  c.onTick(16999, kClock);
  TEST_ASSERT_FALSE(has(c.takeOutgoing(), kPollStop));
  c.onTick(17000, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
  c.onFrame(kGDatStandby, 18000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 19500, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_mist_unit_ends_by_itself() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(70000);
  open(c);
  c.onFrame(kGDatMisting, 2000, kClock);
  c.onFrame(kGDatStandby, 3500, kClock);
  c.onFrame(kGDatStandby, 5000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_FALSE(has(c.takeOutgoing(), kPollStop));
}

void test_mist_request_stop_early() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(70000);
  open(c);
  c.onFrame(kGDatMisting, 2000, kClock);
  c.requestStop();
  c.onTick(2100, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
}

void test_mist_request_stop_before_confirmed_start() {
  Conversation c(Goal::Mist);
  open(c);
  c.requestStop();
  c.onTick(200, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
}

void test_pulse_done_when_started() {
  Conversation c(Goal::Pulse);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollPulse));
  c.onFrame(kGDatMisting, 16000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_TRUE(c.started());
}

void test_pulse_not_started() {
  Conversation c(Goal::Pulse);
  open(c);
  c.onFrame(kGDatStandby, 1600, kClock);
  c.onTick(30109, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onTick(30110, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("not_started", c.error().c_str());
}

void test_stop_confirms_after_two_idle() {
  Conversation c(Goal::Stop);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollStop));
  c.onFrame(kGDatJustStopped, 1600, kClock);  // phase-2 spray still running
  c.onFrame(kGDatStandby, 3100, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 4600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_stop_resends_then_fails() {
  Conversation c(Goal::Stop);
  open(c);  // stop sent at 10, on CiaO
  c.takeOutgoing();
  c.onTick(10110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
  c.onTick(25110, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("stop_not_confirmed", c.error().c_str());
}

void test_scheduler_off() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(false, nullptr);
  auto out = open(c);
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_TRUE(has(out, buildPImpSettings(withAllCyclesActive(s, false), kClock)));
  c.onFrame(kGImp, 1000, kClock);  // still on: not confirmed
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGImpAllOff, 1610, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_scheduler_rehandshakes_after_3s() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(false, nullptr);
  open(c);
  c.onTick(3110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kHandshake));
}

void test_scheduler_on_restores_cached() {
  Settings cached;
  parseGImp(kGImp, cached);
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(true, &cached);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onFrame(kGImpAllOff, 110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), buildPImpSettings(cached, kClock)));
  c.onFrame(kGImp, 1600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_scheduler_on_without_cache_fails() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(true, nullptr);
  open(c);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("no_cached_settings", c.error().c_str());
}

void test_test_pump() {
  Conversation c(Goal::TestPump);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollPump));
  c.onFrame(kGDatStandby, 1600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 3100, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_stop_requested_before_act_never_starts() {
  Conversation c(Goal::Mist);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.requestStop();
  c.onFrame(kGImp, 110, kClock);
  auto out = c.takeOutgoing();
  TEST_ASSERT_FALSE(has(out, kPollMist));
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_FALSE(c.started());
}

void test_stop_during_pending_start_holds_for_start_window() {
  Conversation c(Goal::Mist);
  open(c);  // start sent at 110
  c.requestStop();
  c.onTick(200, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
  c.onFrame(kGDatStandby, 1700, kClock);
  c.onFrame(kGDatStandby, 3200, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatMisting, 16000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 30200, kClock);
  c.onFrame(kGDatStandby, 31700, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_stop_goal_sends_stop_right_after_handshake() {
  Conversation c(Goal::Stop);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  auto out = c.takeOutgoing();
  TEST_ASSERT_TRUE(has(out, kHandshake));
  TEST_ASSERT_TRUE(has(out, kPollStop));
}

void test_pending_start_stop_fails_after_extended_deadline() {
  Conversation c(Goal::Mist);
  open(c);  // start sent at 110
  c.requestStop();
  c.onTick(200, kClock);
  c.takeOutgoing();
  c.onTick(110 + 30000 + 25000 - 1, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onTick(110 + 30000 + 25000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("stop_not_confirmed", c.error().c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_refresh_happy_path);
  RUN_TEST(test_greeting_timeout);
  RUN_TEST(test_no_g_imp_still_acts_after_wait);
  RUN_TEST(test_mist_timed_stop);
  RUN_TEST(test_mist_unit_ends_by_itself);
  RUN_TEST(test_mist_request_stop_early);
  RUN_TEST(test_mist_request_stop_before_confirmed_start);
  RUN_TEST(test_pulse_done_when_started);
  RUN_TEST(test_pulse_not_started);
  RUN_TEST(test_stop_confirms_after_two_idle);
  RUN_TEST(test_stop_resends_then_fails);
  RUN_TEST(test_scheduler_off);
  RUN_TEST(test_scheduler_rehandshakes_after_3s);
  RUN_TEST(test_scheduler_on_restores_cached);
  RUN_TEST(test_scheduler_on_without_cache_fails);
  RUN_TEST(test_test_pump);
  RUN_TEST(test_stop_requested_before_act_never_starts);
  RUN_TEST(test_stop_during_pending_start_holds_for_start_window);
  RUN_TEST(test_stop_goal_sends_stop_right_after_handshake);
  RUN_TEST(test_pending_start_stop_fails_after_extended_deadline);
  return UNITY_END();
}
