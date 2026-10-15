#include "net.h"

#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>

#include "eventlog.h"

// Waveshare ESP32-S3-ETH, W5500 on SPI (wiki pinout)
static constexpr int kEthCs = 14, kEthIrq = 10, kEthRst = 9, kEthSck = 13, kEthMiso = 12, kEthMosi = 11;
static constexpr const char* kZhaltSsid = "ZHALT-EVO_61B3z";
static volatile bool s_ethUp = false;

static void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_GOT_IP:
      s_ethUp = true;
      elog::add("eth up %s", ETH.localIP().toString().c_str());
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
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  ETH.begin(ETH_PHY_W5500, 1, kEthCs, kEthIrq, kEthRst, SPI2_HOST, kEthSck, kEthMiso, kEthMosi);
  ETH.setHostname("zhalt-bridge");
  configTzTime("<+04>-4", "10.0.0.1", "pool.ntp.org", "time.google.com");
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

bool net::joinZhalt(uint32_t timeoutMs, int& rssi, uint32_t& joinMs) {
  uint32_t t0 = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(kZhaltSsid);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  while (WiFi.status() == WL_CONNECTED && WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < timeoutMs)
    delay(50);
  ETH.setDefault();  // keep LAN/MQTT on Ethernet
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    elog::add("zhalt join failed (status %d)", int(WiFi.status()));
    leaveZhalt();
    return false;
  }
  rssi = WiFi.RSSI();
  joinMs = millis() - t0;
  elog::add("zhalt joined in %lu ms, rssi %d, ip %s", (unsigned long)joinMs, rssi, WiFi.localIP().toString().c_str());
  return true;
}

void net::leaveZhalt() {
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  ETH.setDefault();
}
