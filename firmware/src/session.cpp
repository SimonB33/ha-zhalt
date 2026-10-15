// session.cpp
#include "session.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "eventlog.h"
#include "net.h"

session::Outcome session::run(z::Conversation& conv, uint32_t timeoutMs, const std::function<bool()>& stopNow) {
  Outcome o;
  o.joined = net::joinZhalt(kJoinTimeoutMs, o.rssi, o.joinMs);
  if (!o.joined) return o;
  {
    WebSocketsClient ws;
    ws.onEvent([&](WStype_t type, uint8_t* payload, size_t len) {
      uint32_t t = millis();
      if (type == WStype_CONNECTED) {
        o.wsConnected = true;
        conv.onConnected(t);
      } else if (type == WStype_TEXT) {
        conv.onFrame(std::string(reinterpret_cast<char*>(payload), len), t, z::toLocal(net::now()));
      } else if (type == WStype_DISCONNECTED && o.wsConnected) {
        o.wsDropped = true;
      }
    });
    // The library starts with lastConnectionFail = 0 and refuses to connect while
    // millis() - 0 < interval, i.e. for the first 60 s after boot. Cap the interval
    // at the current uptime so the first attempt is immediate; retries stay suppressed.
    ws.setReconnectInterval(min<uint32_t>(60000, millis()));
    ws.begin("172.217.28.1", 81, "/");
    uint32_t t0 = millis();
    while (conv.status() == z::Conversation::Status::Running && millis() - t0 < timeoutMs && !o.wsDropped) {
      if (stopNow && stopNow()) conv.requestStop();
      ws.loop();
      uint32_t t = millis();
      if (!o.wsConnected && t - t0 > kWsConnectTimeoutMs) break;
      if (stopNow && stopNow()) conv.requestStop();
      conv.onTick(t, z::toLocal(net::now()));
      for (const auto& f : conv.takeOutgoing()) ws.sendTXT(f.c_str(), f.size());
      delay(10);
    }
    for (const auto& f : conv.takeOutgoing()) ws.sendTXT(f.c_str(), f.size());  // flush a final stop
    ws.loop();
    ws.disconnect();
  }
  if (!o.wsConnected) {  // diagnostic: which TCP ports does the unit answer on?
    IPAddress gw = WiFi.gatewayIP();
    for (uint16_t port : {81, 80}) {
      WiFiClient c;
      bool open = c.connect(gw, port, 2000);
      c.stop();
      elog::add("probe %s:%u %s", gw.toString().c_str(), port, open ? "open" : "closed");
    }
  }
  net::leaveZhalt();
  return o;
}
