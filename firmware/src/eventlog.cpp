#include "eventlog.h"

#include <Arduino.h>

#include <cstdarg>
#include <deque>
#include <mutex>

#include "net.h"
#include "ztime.h"

static std::deque<std::string> s_lines;
static std::mutex s_mu;
static constexpr size_t kMaxLines = 50;

void elog::add(const char* fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char line[200];
  if (net::clockValid()) {
    z::LocalDateTime t = z::toLocal(net::now());
    snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d %s", t.month, t.day, t.hour, t.minute, t.second, msg);
  } else {
    snprintf(line, sizeof line, "+%lus %s", (unsigned long)(millis() / 1000), msg);
  }
  Serial.println(line);
  std::lock_guard<std::mutex> l(s_mu);
  s_lines.push_back(line);
  while (s_lines.size() > kMaxLines) s_lines.pop_front();
}

std::vector<std::string> elog::recent() {
  std::lock_guard<std::mutex> l(s_mu);
  return {s_lines.begin(), s_lines.end()};
}
