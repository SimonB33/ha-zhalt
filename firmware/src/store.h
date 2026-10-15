#pragma once
#include <string>

namespace store {
void begin();
std::string get(const char* key);
void put(const char* key, const std::string& value);
}  // namespace store
