#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "zsafety.h"

namespace z {

constexpr const char* kTopicCmd = "zhalt/cmd";
constexpr const char* kTopicConfig = "zhalt/config";
constexpr const char* kTopicState = "zhalt/state";
constexpr const char* kTopicStatus = "zhalt/bridge/status";

struct BridgeSnapshot {
  std::string state = "idle";
  bool misting = false;
  int64_t sessionEnd = 0;
  int64_t lastMist = 0;
  std::string lastMistKind;
  std::string lastResult;
  int schedulerOn = -1;  // -1 unknown, 0 off, 1 on
  bool unitValid = false;
  int unitOp = -1;
  int unitActiveCycle = -1;
  std::string unitClock;
  std::string unitFirmware;
  int64_t lastContact = 0;
  bool zhaltReachable = false;
  bool rssiKnown = false;
  int rssi = 0;
  uint32_t joinMs = 0;
  int joinFailures24h = 0;
  uint32_t uptimeS = 0;
  uint32_t freeHeap = 0;
  std::string resetReason;
  std::string fw;
  std::string ip;
  bool passthrough = false;
};

struct DiscoveryMsg {
  std::string topic;
  std::string payload;
};

std::string isoLocal(int64_t epoch);
std::string deriveState(const SafetyState& s, bool mistActive);
std::string stateJson(const BridgeSnapshot& s);
std::vector<DiscoveryMsg> discoveryMessages(const std::string& mac, const std::string& fw, const std::string& ip);

}  // namespace z
