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
#include "mqtt.h"
#include "net.h"
#include "passthrough.h"
#include "store.h"
#include "version.h"

static void safetyTask(void*) {
  for (;;) {
    appTickSafety();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void setup() {
  Serial.begin(115200);
  store::begin();
  net::begin();
  appInit();
  xTaskCreatePinnedToCore(safetyTask, "safety", 6144, nullptr, 2, nullptr, 1);
  mqtt::begin();
  passthrough::begin();
  executorStart();
  elog::add("boot fw %s", ZB_FW_VERSION);
}

void loop() {
  mqtt::loop();
  passthrough::poll();
  if (app.rebootRequested && !app.busy) {
    delay(300);
    mqtt::publishOffline();
    ESP.restart();
  }
  delay(5);
}
