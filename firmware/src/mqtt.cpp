#include "mqtt.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cstring>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "secrets.h"
#include "version.h"
#include "zconfig.h"
#include "zpublish.h"

static WiFiClient s_net;
static PubSubClient s_mqtt(s_net);
static uint32_t s_lastAttempt = 0, s_lastPublish = 0;

static void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  std::string body(reinterpret_cast<char*>(payload), len);
  if (std::strcmp(topic, z::kTopicCmd) == 0) {
    if (body.empty()) return;  // our own retained-clear
    z::Command c;
    std::string err;
    if (!z::parseCommand(body, c, err)) {
      elog::add("bad command (%s)", err.c_str());
      return;
    }
    elog::add("command %s", body.c_str());
    appSubmit(c);
  } else if (std::strcmp(topic, z::kTopicConfig) == 0) {
    appApplyConfig(body);
  }
}

void mqtt::begin() {
  s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  s_mqtt.setBufferSize(2048);
  s_mqtt.setKeepAlive(30);
  s_mqtt.setSocketTimeout(3);
  s_mqtt.setCallback(onMessage);
}

void mqtt::publishOffline() {
  if (s_mqtt.connected()) s_mqtt.publish(z::kTopicStatus, "offline", true);
}

void mqtt::loop() {
  if (!net::ethUp()) return;
  if (!s_mqtt.connected()) {
    if (millis() - s_lastAttempt < 5000) return;
    String id = "zhalt-bridge-" + net::mac();
    bool ok = s_mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, z::kTopicStatus, 1, true, "offline");
    s_lastAttempt = millis();  // back-off starts after the attempt returns
    if (!ok) {
      elog::add("mqtt connect failed rc=%d", s_mqtt.state());
      return;
    }
    s_mqtt.publish(z::kTopicStatus, "online", true);
    // A retained command would replay on every reconnect: clear it before subscribing.
    s_mqtt.publish(z::kTopicCmd, "", true);
    for (const auto& m : z::discoveryMessages(net::mac().c_str(), ZB_FW_VERSION, net::ethIp().c_str()))
      if (!s_mqtt.publish(m.topic.c_str(), m.payload.c_str(), true))
        elog::add("discovery publish failed: %s", m.topic.c_str());
    s_mqtt.subscribe(z::kTopicCmd, 1);
    s_mqtt.subscribe(z::kTopicConfig, 1);
    elog::add("mqtt connected");
    app.dirty = true;
  }
  s_mqtt.loop();
  if (app.dirty.exchange(false) || millis() - s_lastPublish > 60000) {
    std::string js = appStateJson();
    if (s_mqtt.publish(z::kTopicState, js.c_str(), true)) {
      s_lastPublish = millis();
    } else {
      elog::add("state publish failed");
      app.dirty = true;
    }
  }
}
