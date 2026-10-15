// session.h
#pragma once
#include <cstdint>
#include <functional>

#include "zconversation.h"

namespace session {
constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr uint32_t kWsConnectTimeoutMs = 8000;

struct Outcome {
  bool joined = false;
  bool wsConnected = false;
  bool wsDropped = false;
  int rssi = 0;
  uint32_t joinMs = 0;
};

// Join the Zhalt AP, run the conversation until it finishes or times out, then leave.
// stopNow is polled every loop; true asks the conversation to stop the unit.
Outcome run(z::Conversation& conv, uint32_t timeoutMs, const std::function<bool()>& stopNow);
}  // namespace session
