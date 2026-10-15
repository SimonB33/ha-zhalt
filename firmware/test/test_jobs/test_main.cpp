#include <unity.h>
#include "zjobs.h"

using namespace z;

void setUp() {}
void tearDown() {}

static Job J(JobType t) {
  Job j;
  j.type = t;
  return j;
}

void test_fifo() {
  JobQueue q;
  q.push(J(JobType::Refresh));
  q.push(J(JobType::Mist));
  Job out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_TRUE(out.type == JobType::Refresh);
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_TRUE(out.type == JobType::Mist);
  TEST_ASSERT_FALSE(q.pop(out));
}

void test_stop_jumps_queue() {
  JobQueue q;
  q.push(J(JobType::Refresh));
  q.push(J(JobType::TestJoin));
  q.push(J(JobType::Stop));
  Job out;
  q.pop(out);
  TEST_ASSERT_TRUE(out.type == JobType::Stop);
}

void test_stop_drops_queued_starts() {
  JobQueue q;
  q.push(J(JobType::Mist));
  q.push(J(JobType::Refresh));
  q.push(J(JobType::Pulse));
  q.push(J(JobType::Stop));
  TEST_ASSERT_EQUAL(2, (int)q.size());
  TEST_ASSERT_FALSE(q.contains(JobType::Mist));
  TEST_ASSERT_FALSE(q.contains(JobType::Pulse));
  TEST_ASSERT_TRUE(q.contains(JobType::Refresh));
}

void test_duplicate_stop_collapses() {
  JobQueue q;
  q.push(J(JobType::Stop));
  q.push(J(JobType::Stop));
  TEST_ASSERT_EQUAL(1, (int)q.size());
}

void test_full_queue_rejects_but_stop_still_fits() {
  JobQueue q;
  for (size_t i = 0; i < JobQueue::kMax; ++i) TEST_ASSERT_TRUE(q.push(J(JobType::Refresh)));
  TEST_ASSERT_FALSE(q.push(J(JobType::Refresh)));
  TEST_ASSERT_TRUE(q.push(J(JobType::Stop)));
  Job out;
  q.pop(out);
  TEST_ASSERT_TRUE(out.type == JobType::Stop);
}

void test_job_fields_carried() {
  JobQueue q;
  Job j = J(JobType::Pulse);
  j.minutes = 30;
  j.resume = true;
  q.push(j);
  Job out;
  q.pop(out);
  TEST_ASSERT_EQUAL(30, out.minutes);
  TEST_ASSERT_TRUE(out.resume);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifo);
  RUN_TEST(test_stop_jumps_queue);
  RUN_TEST(test_stop_drops_queued_starts);
  RUN_TEST(test_duplicate_stop_collapses);
  RUN_TEST(test_full_queue_rejects_but_stop_still_fits);
  RUN_TEST(test_job_fields_carried);
  return UNITY_END();
}
