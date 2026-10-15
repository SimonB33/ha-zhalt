#include "zprotocol.h"

#include <cstdlib>
#include <sstream>

namespace z {

const char* const kCycleLabels[kNumCycles] = {"C1", "C2", "C3", "C4", "C5", "C6", "CS", "CE", "CJ"};

static const char* const kPassword = "freezanz";
static const char* const kLang = "en-US";

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

static bool toInt(const std::vector<std::string>& t, size_t i, int& out) {
  if (i >= t.size()) return false;
  const char* s = t[i].c_str();
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = int(v);
  return true;
}

static int intAt(const std::vector<std::string>& t, size_t i, int fallback) {
  int v;
  return toInt(t, i, v) ? v : fallback;
}

static int labelIndex(const std::string& label) {
  for (int i = 0; i < kNumCycles; ++i)
    if (label == kCycleLabels[i]) return i;
  return -1;
}

bool parseGImp(const std::string& text, Settings& out) {
  auto t = tokenize(text);
  if (t.empty() || t[0] != "G_imp") return false;
  Settings s;
  s.raw = text;
  s.machine_type = intAt(t, 1, 1);
  s.progcic_type = intAt(t, 3, 0);
  s.firmware_minor = t.size() > 5 ? t[5] : "";
  bool seen[kNumCycles] = {};
  size_t i = 7;
  while (i < t.size()) {
    int idx = labelIndex(t[i]);
    if (idx < 0) break;
    bool numbered = idx < kNumNumbered;
    size_t need = numbered ? 10 : 8;
    if (i + need >= t.size()) break;
    int f[10] = {};
    for (size_t k = 0; k < need; ++k)
      if (!toInt(t, i + 1 + k, f[k])) return false;
    Cycle& c = s.cycles[idx];
    if (numbered) {
      c = Cycle{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]};
    } else {
      c = Cycle{f[0], f[1], 0, 0, f[2], f[3], f[4], f[5], f[6], f[7]};
    }
    seen[idx] = true;
    i += 1 + need;
  }
  s.valid = true;
  for (bool b : seen) s.valid = s.valid && b;
  out = s;
  return true;
}

bool parseGDat(const std::string& text, Dat& out) {
  auto t = tokenize(text);
  if (t.empty() || t[0] != "G_dat" || t.size() < 53) return false;
  Dat d;
  d.valid = true;
  d.tick = intAt(t, 1, -1);
  d.stop_today = intAt(t, 21, -1);
  d.dev_day = intAt(t, 27, 0);
  d.dev_month = intAt(t, 28, 0);
  d.dev_year = intAt(t, 29, 0);
  d.dev_hour = intAt(t, 30, 0);
  d.dev_minute = intAt(t, 31, 0);
  d.dev_second = intAt(t, 32, 0);
  d.op_mode = intAt(t, 38, -1);
  d.active_cycle = intAt(t, 44, -1);
  d.cycle_state = intAt(t, 45, -1);
  d.planned = intAt(t, 48, 0);
  d.elapsed = intAt(t, 52, 0);
  out = d;
  return true;
}

std::string buildPDat(Action a) {
  int flags[10] = {};
  switch (a) {
    case Action::Mist: flags[0] = 1; break;
    case Action::Pulse: flags[1] = 1; break;
    case Action::Stop: flags[2] = 1; break;
    case Action::StopDay: flags[3] = 1; break;
    case Action::TestPump: flags[4] = 1; break;
    case Action::TestLed: flags[5] = 1; break;
    case Action::TestBuzzer: flags[6] = 1; break;
    case Action::None: break;
  }
  std::string s = "P_dat;1920;1080;";
  for (int f : flags) {
    s += std::to_string(f);
    s += ';';
  }
  s += "1;0;0;";  // page_open, snd_test_send, snd_set_send
  return s;
}

static std::string dateHead(const LocalDateTime& n) {
  int chk = n.day + n.month + n.year + n.hour + n.minute + n.second;
  return std::to_string(n.weekday) + ";" + std::to_string(n.day) + ";" + std::to_string(n.month) + ";" +
         std::to_string(n.year) + ";" + std::to_string(n.hour) + ";" + std::to_string(n.minute) + ";" +
         std::to_string(n.second) + ";" + kLang + ";" + std::to_string(chk) + ";";
}

std::string buildPImpHandshake(const LocalDateTime& n) { return "P_imp;" + dateHead(n) + "A;0;"; }

std::string buildPImpSettings(const Settings& s, const LocalDateTime& n) {
  std::string out = "P_imp;" + dateHead(n) + "B;" + std::to_string(s.progcic_type) + ";";
  for (int i = 0; i < kNumCycles; ++i) {
    const Cycle& c = s.cycles[i];
    // Label and act are joined with a space, the rest with semicolons (device web UI post_impo()).
    out += std::string(kCycleLabels[i]) + " " + std::to_string(c.act) + ";";
    std::vector<int> f;
    if (i < kNumNumbered)
      f = {c.mode, c.start_hour, c.start_minute, c.days_bitmap, c.duration_seconds,
           c.end_hour, c.end_minute, c.work_seconds, c.pause_minutes};
    else
      f = {c.mode, c.days_bitmap, c.duration_seconds, c.end_hour, c.end_minute, c.work_seconds, c.pause_minutes};
    for (int v : f) out += std::to_string(v) + ";";
  }
  int machine = s.machine_type == 0 ? 1 : s.machine_type;
  // vbatcalib, cicliplus, change_machinetype, machine_type, psw_changed, psw, lingua, captive, chan, secure_save
  out += "0;" + std::to_string(cicliplus(s)) + ";0;" + std::to_string(machine) + ";0;" + kPassword + ";0;0;0;1;";
  return out;
}

long cicliplus(const Settings& s) {
  auto en = [&](int idx) { return s.cycles[idx].act ? 1L : 0L; };
  // C4, C5, C6, CS, CE, CJ
  return 1000000L * en(3) + 100000L * en(4) + 10000L * en(5) + 1000L * en(6) + 100L * en(7) + 10L * en(8);
}

Settings withAllCyclesActive(const Settings& s, bool active) {
  Settings out = s;
  for (auto& c : out.cycles) c.act = active ? 1 : 0;
  return out;
}

bool anyCycleActive(const Settings& s) {
  for (const auto& c : s.cycles)
    if (c.act) return true;
  return false;
}

std::string opModeName(int op) {
  switch (op) {
    case kOpStopped: return "Stopped";
    case kOpStandby: return "Standby";
    case kOpMisting: return "Misting";
    default: return "Unknown(" + std::to_string(op) + ")";
  }
}

std::string activeCycleName(int id) {
  if (id == 0) return "None";
  if (id == 12) return "Manual";
  if (id >= 1 && id <= 9) return "Cycle" + std::to_string(id);
  return "Unknown(" + std::to_string(id) + ")";
}

}  // namespace z
