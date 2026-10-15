// zjobs.h
#pragma once
#include <cstddef>
#include <deque>

namespace z {

enum class JobType { Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Passthrough };

struct Job {
  JobType type = JobType::Refresh;
  int seconds = 0;
  int minutes = 0;
  bool on = false;
  bool resume = false;  // pulse restarted after a blackout: minutes are not clamped
};

// One worker drains this. Stop jumps the queue and cancels queued starts.
class JobQueue {
 public:
  static constexpr size_t kMax = 8;
  bool push(const Job& j);
  bool pop(Job& out);
  size_t size() const { return q_.size(); }
  bool contains(JobType t) const;

 private:
  std::deque<Job> q_;
};

}  // namespace z
