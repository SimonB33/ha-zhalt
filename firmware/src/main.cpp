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

#include "app.h"
#include "eventlog.h"
#include "executor.h"
#include "net.h"
#include "passthrough.h"
#include "store.h"
#include "version.h"

void setup() {
  Serial.begin(115200);
  store::begin();
  net::begin();
  appInit();
  passthrough::begin();
  executorStart();
  elog::add("boot fw %s", ZB_FW_VERSION);
}

void loop() {
  static uint32_t lastTick = 0;
  passthrough::poll();
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    appTickSafety();
  }
  if (app.rebootRequested && !app.busy) {
    delay(300);
    ESP.restart();
  }
  delay(5);
}
