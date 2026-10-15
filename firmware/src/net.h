#pragma once
#include <Arduino.h>

namespace net {
void begin();
bool ethUp();
bool clockValid();
int64_t now();
String ethIp();
String mac();  // lower-case hex, no separators
// Join the Zhalt's open AP. Ethernet stays the default route throughout.
bool joinZhalt(uint32_t timeoutMs, int& rssi, uint32_t& joinMs);
void leaveZhalt();  // radio fully off
}  // namespace net
