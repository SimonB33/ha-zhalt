// ztime.cpp
#include "ztime.h"

namespace z {

// Civil-from-days (H. Hinnant), shifted to local time.
LocalDateTime toLocal(int64_t epoch) {
  int64_t t = epoch + kUtcOffsetSec;
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  int64_t zd = days + 719468;
  int64_t era = (zd >= 0 ? zd : zd - 146096) / 146097;
  unsigned doe = unsigned(zd - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = int64_t(yoe) + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  int wd = int(((days % 7) + 7 + 3) % 7) + 1;  // 1970-01-01 was a Thursday (4)
  return {int(y), int(m), int(d), int(rem / 3600), int((rem % 3600) / 60), int(rem % 60), wd};
}

int localMinuteOfDay(int64_t epoch) {
  int64_t s = (epoch + kUtcOffsetSec) % 86400;
  if (s < 0) s += 86400;
  return int(s / 60);
}

}  // namespace z
