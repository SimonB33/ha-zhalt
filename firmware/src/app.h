#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "zconfig.h"
#include "zjobs.h"
#include "zprotocol.h"
#include "zpublish.h"
#include "zsafety.h"

struct App {
  std::mutex mu;  // guards the non-atomic fields below
  z::JobQueue queue;
  z::Safety safety;
  z::BridgeSnapshot snap;
  z::Settings cachedOriginal;  // unit settings seen with cycles on; restores "scheduler on"
  std::deque<int64_t> joinFailures;
  std::string savedSafety;
  int64_t mistEndEpoch = 0;
  std::atomic<bool> preempt{false};  // a stop is waiting: running job should yield
  std::atomic<bool> dirty{true};     // state changed: publish
  std::atomic<bool> rebootRequested{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> mistActive{false};
};

extern App app;

void appInit();
void appSubmit(const z::Command& c);
void appApplyConfig(const std::string& json);
void appTickSafety();
void appRecordJoin(bool ok, int rssi, uint32_t joinMs);
void appSetResult(const std::string& result);
std::string appStateJson();
