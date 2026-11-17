#include "net.h"

#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>

#include "eventlog.h"
#include "secrets.h"

// Waveshare ESP32-S3-ETH, W5500 on SPI (wiki pinout)
static constexpr int kEthCs = 14, kEthIrq = 10, kEthRst = 9, kEthSck = 13, kEthMiso = 12, kEthMosi = 11;
static constexpr const char* kZhaltSsid = "ZHALT-EVO_61B3z";
static volatile bool s_ethUp = false;
static volatile int s_lastDiscReason = 0;

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_lastDiscReason = info.wifi_sta_disconnected.reason;
}

// After a failed join: is the unit's AP visible from here, and how strong?
static void logZhaltScan() {
  WiFi.disconnect(false, false);
  int n = WiFi.scanNetworks(false, true);
  int best = -1;
  for (int i = 0; i < n; i++)
    if (WiFi.SSID(i) == kZhaltSsid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  if (best >= 0)
    elog::add("scan: %s seen, rssi %d, ch %d (%d networks)", kZhaltSsid, int(WiFi.RSSI(best)), int(WiFi.channel(best)), n);
  else
    elog::add("scan: %s NOT seen (%d networks)", kZhaltSsid, n);
  WiFi.scanDelete();
}

static void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setDefault();
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ETH.setDefault();
      s_ethUp = true;
      elog::add("eth up %s", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      s_ethUp = false;
      elog::add("eth lost ip");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      s_ethUp = false;
      elog::add("eth down");
      break;
    default:
      break;
  }
}

void net::begin() {
  Network.onEvent(onEvent);
  Network.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  ETH.begin(ETH_PHY_W5500, 1, kEthCs, kEthIrq, kEthRst, SPI2_HOST, kEthSck, kEthMiso, kEthMosi);
  ETH.setHostname("zhalt-bridge");
  // IP literals: DNS can stay pointed at the unit's AP after a join, and 10.0.0.1 serves no NTP.
  configTzTime("<+04>-4", "162.159.200.1", "216.239.35.0", "pool.ntp.org");
}

bool net::ethUp() { return s_ethUp; }
bool net::clockValid() { return time(nullptr) > 1700000000; }
int64_t net::now() { return int64_t(time(nullptr)); }
String net::ethIp() { return ETH.localIP().toString(); }

String net::mac() {
  String m = ETH.macAddress();
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

// One join attempt; on failure the STA is left off so the next attempt starts clean.
static bool joinOnce(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  ETH.setDefault();  // before the STA exists, so it can never take the default route
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  s_lastDiscReason = 0;
  WiFi.begin(kZhaltSsid, ZHALT_WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  while (WiFi.status() == WL_CONNECTED && WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < timeoutMs)
    delay(50);
  ETH.setDefault();  // keep LAN/MQTT on Ethernet
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// The unit's AP is weak from the bridge (-78..-87 dBm), so single attempts sometimes
// time out at auth (reason 2). Retry a few times; good joins take ~2-4 s.
// Worst case: kJoinAttempts * timeoutMs + pauses + one scan (~3 s).
static constexpr int kJoinAttempts = 3;
static constexpr uint32_t kJoinRetryPauseMs = 2000;

bool net::joinZhalt(uint32_t timeoutMs, int& rssi, uint32_t& joinMs) {
  uint32_t t0 = millis();
  for (int attempt = 1; attempt <= kJoinAttempts; attempt++) {
    if (joinOnce(timeoutMs)) {
      rssi = WiFi.RSSI();
      joinMs = millis() - t0;
      elog::add("zhalt joined in %lu ms (attempt %d), rssi %d, ip %s, gw %s", (unsigned long)joinMs, attempt, rssi,
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
      return true;
    }
    elog::add("zhalt join failed (attempt %d/%d, status %d, reason %d)", attempt, kJoinAttempts, int(WiFi.status()),
              int(s_lastDiscReason));
    if (attempt < kJoinAttempts) {
      leaveZhalt();
      delay(kJoinRetryPauseMs);
    }
  }
  logZhaltScan();
  leaveZhalt();
  return false;
}

void net::leaveZhalt() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  ETH.setDefault();
}
