#include <Arduino.h>
// Library headers are included here so PlatformIO's dependency finder links
// the pinned libraries (and the Arduino core libs they need) into the build.
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

void setup() { Serial.begin(115200); }
void loop() { delay(1000); }
