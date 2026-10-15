#pragma once
#include <string>
#include <vector>

namespace elog {
void add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recent();
}  // namespace elog
