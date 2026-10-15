#include <Arduino.h>
// Library headers are kept so PlatformIO's dependency finder links the pinned
// libraries (and the Arduino core libs they need). Removing them breaks the
// build: the finder then pulls in the core WebServer lib without its deps.
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "eventlog.h"
#include "net.h"
#include "store.h"

void setup() {
  Serial.begin(115200);
  store::begin();
  net::begin();
  elog::add("boot");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    elog::add("eth=%d ip=%s clock=%d mac=%s", net::ethUp(), net::ethIp().c_str(), net::clockValid(),
              net::mac().c_str());
  }
  delay(10);
}
