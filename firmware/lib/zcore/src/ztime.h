// ztime.h
#pragma once
#include <cstdint>

namespace z {

constexpr int64_t kUtcOffsetSec = 4 * 3600;  // Asia/Dubai, no DST

struct LocalDateTime {
  int year, month, day, hour, minute, second;
  int weekday;  // Monday=1 .. Sunday=7
};

LocalDateTime toLocal(int64_t epoch);
int localMinuteOfDay(int64_t epoch);

}  // namespace z
