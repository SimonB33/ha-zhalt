#pragma once
#include <string>

namespace z {

struct Window {
  int startMin = -1;  // minute of day, local
  int endMin = -1;    // exclusive; may be < startMin to cross midnight
  bool enabled() const { return startMin >= 0 && endMin >= 0 && startMin != endMin; }
  bool contains(int minuteOfDay) const;
};

constexpr int kNumWindows = 3;

struct Config {
  int mistSeconds = 70;
  int resumeMinMinutes = 10;
  Window windows[kNumWindows];
};

Config defaultConfig();
bool parseHHMM(const std::string& s, int& minutes);
bool parseConfig(const std::string& json, Config& inOut, std::string& err);
std::string configToJson(const Config& c);

enum class CmdType { Invalid, Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Reboot };

struct Command {
  CmdType type = CmdType::Invalid;
  int seconds = 0;  // mist; 0 = use config
  int minutes = 0;  // pulse; 0 = default
  bool on = false;  // scheduler
};

bool parseCommand(const std::string& json, Command& out, std::string& err);

}  // namespace z
