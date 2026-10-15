#include <unity.h>
#include <cstring>
#include "../fixtures.h"

void setUp() {}
void tearDown() {}

void test_fixtures_present() { TEST_ASSERT_EQUAL(0, std::strncmp(kGImp, "G_imp", 5)); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fixtures_present);
  return UNITY_END();
}
