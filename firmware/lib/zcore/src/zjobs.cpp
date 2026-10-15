// zjobs.cpp
#include "zjobs.h"

namespace z {

bool JobQueue::contains(JobType t) const {
  for (const auto& j : q_)
    if (j.type == t) return true;
  return false;
}

bool JobQueue::push(const Job& j) {
  if (j.type == JobType::Stop) {
    for (auto it = q_.begin(); it != q_.end();) {
      if (it->type == JobType::Mist || it->type == JobType::Pulse)
        it = q_.erase(it);
      else
        ++it;
    }
    if (!contains(JobType::Stop)) q_.push_front(j);
    return true;
  }
  if (q_.size() >= kMax) return false;
  q_.push_back(j);
  return true;
}

bool JobQueue::pop(Job& out) {
  if (q_.empty()) return false;
  out = q_.front();
  q_.pop_front();
  return true;
}

}  // namespace z
