#pragma once
#include <string>
#include <vector>

#include "ztime.h"

namespace z {

constexpr const char* kGreeting = "CiaO";
constexpr int kNumCycles = 9;
constexpr int kNumNumbered = 6;  // C1..C6; CS, CE, CJ follow
extern const char* const kCycleLabels[kNumCycles];

constexpr int kOpStopped = 6;
constexpr int kOpStandby = 7;
constexpr int kOpMisting = 9;

struct Cycle {
  int act = 0, mode = 0, start_hour = 0, start_minute = 0, days_bitmap = 0,
      duration_seconds = 0, end_hour = 0, end_minute = 0, work_seconds = 0, pause_minutes = 0;
};

struct Settings {
  bool valid = false;  // true only when all 9 cycles were parsed
  int machine_type = 1;
  int progcic_type = 0;
  std::string firmware_minor;
  Cycle cycles[kNumCycles];
  std::string raw;
};

struct Dat {
  bool valid = false;
  int tick = -1, op_mode = -1, active_cycle = -1, cycle_state = -1;
  int planned = 0, elapsed = 0, stop_today = -1;
  int dev_year = 0, dev_month = 0, dev_day = 0, dev_hour = 0, dev_minute = 0, dev_second = 0;
  // cycle_state 1 = phase-1 spray, 2 = phase-2 spray (reported with op 6)
  bool pumpRunning() const { return cycle_state == 1 || cycle_state == 2; }
  bool spraying() const { return op_mode == kOpMisting || pumpRunning(); }
};

enum class Action { None, Mist, Pulse, Stop, StopDay, TestPump, TestLed, TestBuzzer };

std::vector<std::string> tokenize(const std::string& text);
bool parseGImp(const std::string& text, Settings& out);
bool parseGDat(const std::string& text, Dat& out);
std::string buildPDat(Action action);
std::string buildPImpHandshake(const LocalDateTime& now);
std::string buildPImpSettings(const Settings& s, const LocalDateTime& now);
long cicliplus(const Settings& s);
Settings withAllCyclesActive(const Settings& s, bool active);
bool anyCycleActive(const Settings& s);
std::string opModeName(int op);
std::string activeCycleName(int id);

}  // namespace z
