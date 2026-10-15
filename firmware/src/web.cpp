// web.cpp
#include "web.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "secrets.h"
#include "zconfig.h"

// Refuse to build with the placeholder web password.
static constexpr bool sameStr(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || sameStr(a + 1, b + 1));
}
static_assert(!sameStr(WEB_PASS, "change-me"), "set WEB_PASS in include/secrets.h (still the default)");

static AsyncWebServer s_server(80);

static std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static bool s_updateOk = false, s_updateSeen = false;

static bool sameOrigin(AsyncWebServerRequest* r) {
  String base = "http://" + net::ethIp();
  if (r->hasHeader("Origin")) return r->getHeader("Origin")->value() == base;
  if (r->hasHeader("Referer")) return r->getHeader("Referer")->value().startsWith(base + "/");
  return true;  // no browser headers (curl)
}

static bool authed(AsyncWebServerRequest* r) {
  if (!sameOrigin(r)) {
    r->send(403, "text/plain", "forbidden");
    return false;
  }
  if (r->authenticate(WEB_USER, WEB_PASS)) return true;
  r->requestAuthentication();
  return false;
}

static std::string pageHtml() {
  JsonDocument doc;
  deserializeJson(doc, appStateJson());
  std::string h =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<meta http-equiv=refresh content=5><title>Zhalt Bridge</title>"
      "<style>body{font-family:system-ui;margin:16px;max-width:720px}td{padding:2px 8px;"
      "border-bottom:1px solid #ddd}pre{background:#f4f4f4;padding:8px;white-space:pre-wrap}"
      "form{display:inline}button{margin:4px}</style></head><body><h1>Zhalt Bridge</h1><table>";
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    std::string v;
    serializeJson(kv.value(), v);
    h += "<tr><td>" + esc(kv.key().c_str()) + "</td><td>" + esc(v) + "</td></tr>";
  }
  h += "</table><p><form method=post action=/test-join><button>Test join</button></form>"
       "<form method=post action=/reboot><button>Reboot</button></form></p>"
       "<h2>Firmware</h2><form method=post action=/update enctype=multipart/form-data>"
       "<input type=file name=firmware><button>Upload</button></form><h2>Log</h2><pre>";
  auto lines = elog::recent();
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) h += esc(*it) + "\n";
  h += "</pre></body></html>";
  return h;
}

static std::string statusJson() {
  JsonDocument doc;
  deserializeJson(doc, appStateJson());
  JsonArray log = doc["log"].to<JsonArray>();
  for (const auto& l : elog::recent()) log.add(l);
  std::string out;
  serializeJson(doc, out);
  return out;
}

void web::begin() {
  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/html", pageHtml().c_str()); });
  s_server.on("/status.json", HTTP_GET,
              [](AsyncWebServerRequest* r) { r->send(200, "application/json", statusJson().c_str()); });
  s_server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    app.rebootRequested = true;
    r->send(200, "text/plain", "rebooting when idle");
  });
  s_server.on("/test-join", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    z::Command c;
    c.type = z::CmdType::TestJoin;
    appSubmit(c);
    r->redirect("/");
  });
  s_server.on(
      "/update", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        if (!authed(r)) return;
        if (!s_updateSeen) {
          r->send(400, "text/plain", "no firmware");
          return;
        }
        bool ok = s_updateOk;
        s_updateSeen = false;
        s_updateOk = false;
        r->send(ok ? 200 : 500, "text/plain", ok ? "ok, rebooting when idle" : "update failed");
        if (ok) app.rebootRequested = true;
      },
      [](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!sameOrigin(r) || !r->authenticate(WEB_USER, WEB_PASS)) return;
        if (index == 0) {
          if (Update.isRunning()) Update.abort();
          s_updateSeen = true;
          s_updateOk = false;
          r->onDisconnect([] {
            if (Update.isRunning()) Update.abort();
          });
          elog::add("firmware upload %s", filename.c_str());
          if (Update.begin(UPDATE_SIZE_UNKNOWN))
            s_updateOk = true;
          else
            elog::add("update begin failed");
        }
        if (!s_updateOk) return;
        if (Update.isRunning() && Update.write(data, len) != len) {
          elog::add("update write failed");
          Update.abort();
          s_updateOk = false;
          return;
        }
        if (final) {
          s_updateOk = Update.end(true);
          if (s_updateOk)
            elog::add("update ok, %u bytes", unsigned(index + len));
          else
            elog::add("update end failed");
        }
      });
  s_server.begin();
}
