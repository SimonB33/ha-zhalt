# Zhalt ESP32 Bridge Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Firmware for the Waveshare ESP32-S3-ETH that talks to the Zhalt itself, joins its Wi-Fi only when a command needs it, enforces safety limits on the board, and exposes everything to HA over MQTT, with a TCP pass-through as a fallback.

**Architecture:** All decision logic lives in a pure C++ library (`lib/zcore`) with no Arduino dependencies: the protocol port, the per-connection conversation state machine, config/command parsing, safety timers, the job queue and the MQTT payload builders. It is unit-tested on the Mac (PlatformIO `native` env, Unity). Thin Arduino glue in `src/` wires it to Ethernet (W5500), on-demand Wi-Fi, a WebSocket client, MQTT, a web page and NVS.

**Tech Stack:** PlatformIO, Arduino-ESP32 3.x (pioarduino platform, ESP-IDF 5), ArduinoJson 7, links2004 WebSockets, PubSubClient, ESPAsyncWebServer/AsyncTCP, Unity.

**Spec:** `docs/superpowers/specs/2026-10-15-esp32-bridge-design.md`

**Scope:** This plan covers the firmware, from the desk bench test through the dry run at the unit. The Home Assistant cut-over (helpers, scripts, automations, dashboards) is a second plan. It is written once the board is on the bench and its real entity IDs exist (Task 13).

## Global Constraints

- Zhalt AP: SSID `ZHALT-EVO_61B3z`, open, unit at `ws://172.217.28.1:81/`. One client at a time.
- The board joins the Zhalt AP **only for the duration of one job**. Radio fully off (`WIFI_OFF`) when idle. Never held for a pulse session.
- Mist cap 120 s, compiled in. Pulse cap from the command, clamped to 10–90 min, default 40. Mist default from config, 70 s.
- Blackout defaults: 05:55–06:15, 07:00–07:20, 17:30–18:30 (local Asia/Dubai, UTC+4, no DST). Resume-after-blackout threshold default 10 min. All overridable from HA via `zhalt/config`.
- Stop retries: 3 retries, 20 s apart, after the first attempt; then state `stop_failed`.
- Start commands are refused until NTP has synced. Stop always works.
- The board never turns the unit's onboard scheduler on by itself.
- MQTT topics: `zhalt/cmd`, `zhalt/config`, `zhalt/state`, `zhalt/bridge/status`. Discovery prefix `homeassistant/`. Entity IDs `<domain>.zhalt_mqtt_<key>`.
- Bridge IP 10.0.0.30 via UDM DHCP reservation (not hard-coded). Hostname `zhalt-bridge`.
- Secrets live in `firmware/include/secrets.h` (gitignored). Never commit it.
- **Process rule (Simon's global CLAUDE.md, repeat verbatim to any subagent that may start servers or monitors):**
  - Only ever stop a process you started in this session, by its exact PID. Record the PID when you start it (`cmd & echo $!`, or the background task id) and stop it with `kill <pid>`.
  - Never use `pkill`, `killall`, `pkill -f <pattern>`, `lsof ... | xargs kill`, `kill $(lsof ...)`, or any kill selected by name, pattern or port. Port and pattern matches hit unrelated processes.
  - If a port is busy and you did not start the process holding it, do not kill it. Pick another port or ask.
  - Never quit, restart or shut down apps, VMs, simulators you did not boot, Docker/OrbStack or the machine without asking first.
- **No misting without Simon's explicit OK.** Nothing in this plan sends `mist`, `pulse` or `test_pump` to the real unit.

## Review Focus

1. **A retained message on `zhalt/cmd`** (someone publishes with retain) must not replay a command on every reconnect. The board clears the retained command before subscribing. Checked on the bench in Task 11, step 6.
2. **A reboot or power loss mid pulse session** must still produce the Stop at the cap or at a blackout. Safety state persists to NVS and restores. Covered by the Task 4 test `test_restore_after_reboot_still_stops`.
3. **Bad config from HA** must be rejected with the previous config kept: `"5:5"`, `"25:00"`, mist 200, a 4th window, non-JSON. Floats like `70.0` must be accepted. Covered by Task 3 tests.
4. **Stop while a start is queued or in flight:**
   - A queued mist or pulse is dropped (Task 5 `test_stop_drops_queued_starts`).
   - A mist that is starting is stopped (Task 2 `test_mist_request_stop_early`).
5. **A start during a running pulse session, inside a blackout, or before NTP sync** is refused with a reason (`pulse_active`, `blackout`, `clock_not_set`). Covered by the Task 4 `test_gate_*` tests.

Known, accepted gap: the pass-through (fallback for the HACS integration) is **not** gated by board safety. The integration keeps its own gates. Pass-through is off the main path.

---

## File Structure

```
firmware/
  platformio.ini
  .gitignore                     .pio/, include/secrets.h
  include/
    secrets.h.example            MQTT + web credentials template
    version.h                    ZB_FW_VERSION
  lib/zcore/src/                 pure C++, unit-tested on the Mac
    ztime.h / ztime.cpp          epoch -> Dubai local date/time, minute-of-day
    zprotocol.h / .cpp           G_imp/G_dat parsers, P_dat/P_imp builders (port of protocol.py)
    zconversation.h / .cpp       one WebSocket session as a state machine (frames in, frames out)
    zconfig.h / .cpp             Config (blackouts, mist secs, resume threshold) + Command parsing
    zsafety.h / .cpp             caps, blackout stop/resume, stop retries, gates, persistence
    zjobs.h / .cpp               single-worker job queue, Stop jumps the queue
    zpublish.h / .cpp            state JSON, derived state, HA discovery payloads
  src/                           Arduino glue, verified by compile + bench
    main.cpp                     setup/loop
    app.h / app.cpp              shared state + mutex, command intake, safety tick, state JSON
    executor.h / executor.cpp    worker task: runs jobs, maps results to safety
    session.h / session.cpp      join AP + WebSocket + drive a Conversation + leave
    net.h / net.cpp              W5500 Ethernet, NTP, on-demand Zhalt Wi-Fi
    mqtt.h / mqtt.cpp            PubSubClient, discovery, cmd/config intake, state publish
    web.h / web.cpp              status page, /status.json, reboot, test-join, firmware upload
    passthrough.h / .cpp         TCP :81 on Ethernet spliced to the unit
    store.h / store.cpp          NVS (Preferences) string store
    eventlog.h / eventlog.cpp    50-line ring log + Serial
  test/
    fixtures.h                   captured unit frames (from tests/fixtures/)
    test_protocol/test_main.cpp
    test_conversation/test_main.cpp
    test_config/test_main.cpp
    test_safety/test_main.cpp
    test_jobs/test_main.cpp
    test_publish/test_main.cpp
```

All commands below run from `/Volumes/External/Apps/Home Assistant/Zhalt/firmware` unless stated otherwise.

---

### Task 0: Toolchain and project scaffold

**Files:**
- Create: `firmware/platformio.ini`, `firmware/.gitignore`, `firmware/include/secrets.h.example`, `firmware/include/version.h`, `firmware/src/main.cpp`, `firmware/test/fixtures.h`, `firmware/test/test_smoke/test_main.cpp`

**Interfaces:**
- Produces: envs `native` (tests) and `esp32s3` (board); `test/fixtures.h` constants `kGImp`, `kGImpAllOff`, `kGDatStandby`, `kGDatMisting`, `kGDatJustStopped`.

- [ ] **Step 1: Create a branch and install PlatformIO**

```bash
cd "/Volumes/External/Apps/Home Assistant/Zhalt" && git checkout -b esp32-bridge
brew install platformio
pio --version
```
Expected: `PlatformIO Core, version 6.x`.

- [ ] **Step 2: Write `firmware/platformio.ini`**

```ini
[platformio]
default_envs = esp32s3

[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -Wall
lib_deps = bblanchon/ArduinoJson@7.2.1

[env:esp32s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
monitor_speed = 115200
build_flags = -DARDUINO_USB_CDC_ON_BOOT=1 -DBOARD_HAS_PSRAM
lib_deps =
  bblanchon/ArduinoJson@7.2.1
  links2004/WebSockets@2.6.1
  knolleary/PubSubClient@2.8
  ESP32Async/AsyncTCP@3.3.2
  ESP32Async/ESPAsyncWebServer@3.6.0
test_ignore = *
```

If a pinned library version fails to resolve, use the nearest existing release of that library. Record the change in the commit message.

- [ ] **Step 3: Write `.gitignore`, `include/version.h`, `include/secrets.h.example`**

`firmware/.gitignore`:
```
.pio/
include/secrets.h
```

`firmware/include/version.h`:
```cpp
#pragma once
#define ZB_FW_VERSION "0.1.0"
```

`firmware/include/secrets.h.example`:
```cpp
#pragma once
// Copy to secrets.h (gitignored) and fill in.
#define MQTT_HOST "10.0.0.9"
#define MQTT_PORT 1883
#define MQTT_USER "zhalt-bridge"
#define MQTT_PASS "change-me"
#define WEB_USER "admin"
#define WEB_PASS "change-me"
```

Then `cp include/secrets.h.example include/secrets.h` (placeholder values are fine until Task 11).

- [ ] **Step 4: Write a placeholder `src/main.cpp`**

```cpp
#include <Arduino.h>

void setup() { Serial.begin(115200); }
void loop() { delay(1000); }
```

- [ ] **Step 5: Write `test/fixtures.h`** (frames captured from the real unit; copied from `tests/fixtures/`)

```cpp
#pragma once
// Captured from the real unit (tests/fixtures/*.txt in the integration).
static const char* const kGImp =
    "G_imp 1 0 1 2 08 A C1 1 1 5 45 127 70 8 0 10 5 C2 1 2 6 0 127 70 7 0 10 5 "
    "C3 1 1 18 30 127 70 20 0 10 5 C4 1 2 19 0 127 70 20 30 10 5 C5 0 1 19 0 127 70 20 30 10 5 "
    "C6 0 1 21 0 127 70 22 30 10 5 CS 1 1 127 70 1 30 10 5 CE 0 1 127 70 1 30 10 5 "
    "CJ 0 1 127 70 1 30 10 5 0 0 -776 1001000 0 208h 07/12/2022 0 7 1 7 8 251 29 4 0 0 "
    "09/09/2023 09/09/2023 22/02/2024 1 4948 2088 0 25 0 425 422 0 freezanz 1 0 9";
// Same frame with every cycle's act flag set to 0.
static const char* const kGImpAllOff =
    "G_imp 1 0 1 2 08 A C1 0 1 5 45 127 70 8 0 10 5 C2 0 2 6 0 127 70 7 0 10 5 "
    "C3 0 1 18 30 127 70 20 0 10 5 C4 0 2 19 0 127 70 20 30 10 5 C5 0 1 19 0 127 70 20 30 10 5 "
    "C6 0 1 21 0 127 70 22 30 10 5 CS 0 1 127 70 1 30 10 5 CE 0 1 127 70 1 30 10 5 "
    "CJ 0 1 127 70 1 30 10 5 0 0 -776 0 0 208h 07/12/2022 0 7 1 7 8 251 29 4 0 0 "
    "09/09/2023 09/09/2023 22/02/2024 1 4948 2088 0 25 0 425 422 0 freezanz 1 0 9";
static const char* const kGDatStandby =
    "G_dat 8 0 0 0 0 0 0 0 0 0 0 0 0 354 1 0 1172 1095 0 0 0 1 0 1 0 0 7 5 2026 7 41 1 4 0 12 "
    "46 24 7 20 331 0 0 B 1 0 0 0 10 0 300 0 0 0 0 0 0 4 1 3 0";
static const char* const kGDatMisting =
    "G_dat 4 0 0 0 0 0 0 1 1 1 1 1 0 354 1 0 1172 1095 0 0 0 1 0 1 0 0 7 5 2026 7 45 47 4 0 12 "
    "46 24 9 36 333 0 0 B 12 1 1 0 70 0 300 0 200 0 0 0 0 4 1 3 0";
static const char* const kGDatJustStopped =
    "G_dat 7 0 0 0 0 0 0 1 1 1 1 1 0 354 1 0 1172 1095 0 0 1 1 0 1 0 0 7 5 2026 7 45 47 4 0 12 "
    "46 24 6 36 338 0 0 B 12 2 1 1 70 0 300 0 0 0 1 0 0 4 1 3 0";
```

- [ ] **Step 6: Write a smoke test `test/test_smoke/test_main.cpp`**

```cpp
#include <unity.h>
#include <cstring>
#include "../fixtures.h"

void setUp() {}
void tearDown() {}

void test_fixtures_present() { TEST_ASSERT_EQUAL(0, std::strncmp(kGImp, "G_imp", 5)); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fixtures_present);
  return UNITY_END();
}
```

- [ ] **Step 7: Run tests and a board build**

Run: `pio test -e native`
Expected: `test_smoke ... PASSED`.

Run: `pio run -e esp32s3`
Expected: `SUCCESS` (first run downloads the toolchain, several minutes).

- [ ] **Step 8: Commit**

```bash
git add firmware && git commit -m "firmware: PlatformIO scaffold, fixtures, smoke test

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 1: Local time + protocol port (`ztime`, `zprotocol`)

**Files:**
- Create: `firmware/lib/zcore/src/ztime.h`, `ztime.cpp`, `zprotocol.h`, `zprotocol.cpp`
- Test: `firmware/test/test_protocol/test_main.cpp`

**Interfaces:**
- Produces:
  - `z::LocalDateTime{year,month,day,hour,minute,second,weekday}` (weekday Monday=1..Sunday=7)
  - `z::LocalDateTime z::toLocal(int64_t epoch)`
  - `int z::localMinuteOfDay(int64_t epoch)`
  - `z::Settings`, `z::Cycle`, `z::Dat` (`pumpRunning()`, `spraying()`)
  - `enum class z::Action {None, Mist, Pulse, Stop, StopDay, TestPump, TestLed, TestBuzzer}`
  - `bool z::parseGImp(const std::string&, Settings&)`, `bool z::parseGDat(const std::string&, Dat&)`
  - `std::string z::buildPDat(Action)`, `z::buildPImpHandshake(const LocalDateTime&)`, `z::buildPImpSettings(const Settings&, const LocalDateTime&)`
  - `long z::cicliplus(const Settings&)`, `Settings z::withAllCyclesActive(const Settings&, bool)`, `bool z::anyCycleActive(const Settings&)`
  - `std::string z::opModeName(int)`, `std::string z::activeCycleName(int)`
  - constants `z::kGreeting`, `z::kOpStopped/kOpStandby/kOpMisting`

- [ ] **Step 1: Write the failing test `test/test_protocol/test_main.cpp`**

Expected strings were generated from the Python `protocol.py` with the same inputs.

```cpp
#include <unity.h>
#include "../fixtures.h"
#include "zprotocol.h"
#include "ztime.h"

using namespace z;

void setUp() {}
void tearDown() {}

// 2026-05-07 08:55:12 Asia/Dubai
static const int64_t kEpochMay7 = 1778129712;

void test_to_local_basic() {
  LocalDateTime t = toLocal(kEpochMay7);
  TEST_ASSERT_EQUAL(2026, t.year);
  TEST_ASSERT_EQUAL(5, t.month);
  TEST_ASSERT_EQUAL(7, t.day);
  TEST_ASSERT_EQUAL(8, t.hour);
  TEST_ASSERT_EQUAL(55, t.minute);
  TEST_ASSERT_EQUAL(12, t.second);
  TEST_ASSERT_EQUAL(4, t.weekday);  // Thursday
}

void test_to_local_leap_day() {
  LocalDateTime t = toLocal(1709236799);  // 2024-02-29 23:59:59 Dubai
  TEST_ASSERT_EQUAL(2024, t.year);
  TEST_ASSERT_EQUAL(2, t.month);
  TEST_ASSERT_EQUAL(29, t.day);
  TEST_ASSERT_EQUAL(23, t.hour);
  TEST_ASSERT_EQUAL(59, t.second);
  TEST_ASSERT_EQUAL(4, t.weekday);
}

void test_minute_of_day() {
  TEST_ASSERT_EQUAL(355, localMinuteOfDay(1792029300));  // 2026-10-15 05:55 Dubai
}

void test_parse_g_imp_header_and_cycles() {
  Settings s;
  TEST_ASSERT_TRUE(parseGImp(kGImp, s));
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL(1, s.machine_type);
  TEST_ASSERT_EQUAL(1, s.progcic_type);
  TEST_ASSERT_EQUAL_STRING("08", s.firmware_minor.c_str());
  const Cycle& c1 = s.cycles[0];
  TEST_ASSERT_EQUAL(1, c1.act);
  TEST_ASSERT_EQUAL(1, c1.mode);
  TEST_ASSERT_EQUAL(5, c1.start_hour);
  TEST_ASSERT_EQUAL(45, c1.start_minute);
  TEST_ASSERT_EQUAL(127, c1.days_bitmap);
  TEST_ASSERT_EQUAL(70, c1.duration_seconds);
  TEST_ASSERT_EQUAL(8, c1.end_hour);
  const Cycle& cs = s.cycles[6];
  TEST_ASSERT_EQUAL(1, cs.act);
  TEST_ASSERT_EQUAL(1, cs.end_hour);
  TEST_ASSERT_EQUAL(30, cs.end_minute);
  TEST_ASSERT_EQUAL(0, s.cycles[4].act);  // C5
  TEST_ASSERT_EQUAL(0, s.cycles[8].act);  // CJ
  TEST_ASSERT_EQUAL_STRING(kGImp, s.raw.c_str());
}

void test_parse_g_imp_rejects_other_frames() {
  Settings s;
  TEST_ASSERT_FALSE(parseGImp("CiaO", s));
  TEST_ASSERT_FALSE(parseGImp("G_dat 1 2 3", s));
}

void test_parse_g_imp_truncated_is_not_valid() {
  Settings s;
  TEST_ASSERT_TRUE(parseGImp("G_imp 1 0 1 2 08 A C1 1 1 5 45 127 70 8 0 10 5", s));
  TEST_ASSERT_FALSE(s.valid);
}

void test_parse_g_dat_states() {
  Dat d;
  TEST_ASSERT_TRUE(parseGDat(kGDatStandby, d));
  TEST_ASSERT_EQUAL(7, d.op_mode);
  TEST_ASSERT_EQUAL(0, d.cycle_state);
  TEST_ASSERT_EQUAL(1, d.active_cycle);
  TEST_ASSERT_EQUAL(2026, d.dev_year);
  TEST_ASSERT_EQUAL(5, d.dev_month);
  TEST_ASSERT_EQUAL(7, d.dev_day);
  TEST_ASSERT_FALSE(d.spraying());

  TEST_ASSERT_TRUE(parseGDat(kGDatMisting, d));
  TEST_ASSERT_EQUAL(9, d.op_mode);
  TEST_ASSERT_EQUAL(12, d.active_cycle);
  TEST_ASSERT_EQUAL(70, d.planned);
  TEST_ASSERT_EQUAL(200, d.elapsed);
  TEST_ASSERT_TRUE(d.spraying());

  // op 6 with cycle_state 2: phase-2 spray can still be running, so it counts as spraying
  TEST_ASSERT_TRUE(parseGDat(kGDatJustStopped, d));
  TEST_ASSERT_EQUAL(6, d.op_mode);
  TEST_ASSERT_EQUAL(2, d.cycle_state);
  TEST_ASSERT_TRUE(d.spraying());
}

void test_parse_g_dat_rejects_short_or_other() {
  Dat d;
  TEST_ASSERT_FALSE(parseGDat("G_dat 1 2 3", d));
  TEST_ASSERT_FALSE(parseGDat("G_imp 1", d));
}

void test_build_p_dat() {
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::None).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;1;0;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Mist).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;1;0;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Pulse).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;1;0;0;0;0;0;0;0;1;0;0;", buildPDat(Action::Stop).c_str());
  TEST_ASSERT_EQUAL_STRING("P_dat;1920;1080;0;0;0;0;1;0;0;0;0;0;1;0;0;", buildPDat(Action::TestPump).c_str());
}

void test_build_handshake() {
  TEST_ASSERT_EQUAL_STRING("P_imp;4;7;5;2026;8;55;12;en-US;2113;A;0;",
                           buildPImpHandshake(toLocal(kEpochMay7)).c_str());
}

void test_build_settings_round_trip() {
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_EQUAL_STRING(
      "P_imp;4;7;5;2026;8;55;12;en-US;2113;B;1;C1 1;1;5;45;127;70;8;0;10;5;C2 1;2;6;0;127;70;7;0;10;5;"
      "C3 1;1;18;30;127;70;20;0;10;5;C4 1;2;19;0;127;70;20;30;10;5;C5 0;1;19;0;127;70;20;30;10;5;"
      "C6 0;1;21;0;127;70;22;30;10;5;CS 1;1;127;70;1;30;10;5;CE 0;1;127;70;1;30;10;5;"
      "CJ 0;1;127;70;1;30;10;5;0;1001000;0;1;0;freezanz;0;0;0;1;",
      buildPImpSettings(s, toLocal(kEpochMay7)).c_str());
}

void test_build_settings_all_off() {
  Settings s;
  parseGImp(kGImp, s);
  Settings off = withAllCyclesActive(s, false);
  TEST_ASSERT_EQUAL_STRING(
      "P_imp;4;7;5;2026;8;55;12;en-US;2113;B;1;C1 0;1;5;45;127;70;8;0;10;5;C2 0;2;6;0;127;70;7;0;10;5;"
      "C3 0;1;18;30;127;70;20;0;10;5;C4 0;2;19;0;127;70;20;30;10;5;C5 0;1;19;0;127;70;20;30;10;5;"
      "C6 0;1;21;0;127;70;22;30;10;5;CS 0;1;127;70;1;30;10;5;CE 0;1;127;70;1;30;10;5;"
      "CJ 0;1;127;70;1;30;10;5;0;0;0;1;0;freezanz;0;0;0;1;",
      buildPImpSettings(off, toLocal(kEpochMay7)).c_str());
  TEST_ASSERT_EQUAL(1, s.cycles[0].act);  // original untouched
}

void test_cicliplus_and_any_active() {
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_EQUAL(1001000L, cicliplus(s));
  TEST_ASSERT_TRUE(anyCycleActive(s));
  Settings off;
  parseGImp(kGImpAllOff, off);
  TEST_ASSERT_EQUAL(0L, cicliplus(off));
  TEST_ASSERT_FALSE(anyCycleActive(off));
}

void test_names() {
  TEST_ASSERT_EQUAL_STRING("Misting", opModeName(9).c_str());
  TEST_ASSERT_EQUAL_STRING("Standby", opModeName(7).c_str());
  TEST_ASSERT_EQUAL_STRING("Stopped", opModeName(6).c_str());
  TEST_ASSERT_EQUAL_STRING("Unknown(3)", opModeName(3).c_str());
  TEST_ASSERT_EQUAL_STRING("Manual", activeCycleName(12).c_str());
  TEST_ASSERT_EQUAL_STRING("None", activeCycleName(0).c_str());
  TEST_ASSERT_EQUAL_STRING("Cycle1", activeCycleName(1).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_to_local_basic);
  RUN_TEST(test_to_local_leap_day);
  RUN_TEST(test_minute_of_day);
  RUN_TEST(test_parse_g_imp_header_and_cycles);
  RUN_TEST(test_parse_g_imp_rejects_other_frames);
  RUN_TEST(test_parse_g_imp_truncated_is_not_valid);
  RUN_TEST(test_parse_g_dat_states);
  RUN_TEST(test_parse_g_dat_rejects_short_or_other);
  RUN_TEST(test_build_p_dat);
  RUN_TEST(test_build_handshake);
  RUN_TEST(test_build_settings_round_trip);
  RUN_TEST(test_build_settings_all_off);
  RUN_TEST(test_cicliplus_and_any_active);
  RUN_TEST(test_names);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_protocol`
Expected: compile error, `zprotocol.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/ztime.h` and `ztime.cpp`**

```cpp
// ztime.h
#pragma once
#include <cstdint>

namespace z {

constexpr int64_t kUtcOffsetSec = 4 * 3600;  // Asia/Dubai, no DST

struct LocalDateTime {
  int year, month, day, hour, minute, second;
  int weekday;  // Monday=1 .. Sunday=7
};

LocalDateTime toLocal(int64_t epoch);
int localMinuteOfDay(int64_t epoch);

}  // namespace z
```

```cpp
// ztime.cpp
#include "ztime.h"

namespace z {

// Civil-from-days (H. Hinnant), shifted to local time.
LocalDateTime toLocal(int64_t epoch) {
  int64_t t = epoch + kUtcOffsetSec;
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  int64_t zd = days + 719468;
  int64_t era = (zd >= 0 ? zd : zd - 146096) / 146097;
  unsigned doe = unsigned(zd - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = int64_t(yoe) + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  int wd = int(((days % 7) + 7 + 3) % 7) + 1;  // 1970-01-01 was a Thursday (4)
  return {int(y), int(m), int(d), int(rem / 3600), int((rem % 3600) / 60), int(rem % 60), wd};
}

int localMinuteOfDay(int64_t epoch) {
  int64_t s = (epoch + kUtcOffsetSec) % 86400;
  if (s < 0) s += 86400;
  return int(s / 60);
}

}  // namespace z
```

- [ ] **Step 4: Write `lib/zcore/src/zprotocol.h`**

```cpp
#pragma once
#include <string>
#include <vector>

#include "ztime.h"

namespace z {

constexpr const char* kGreeting = "CiaO";
constexpr int kNumCycles = 9;
constexpr int kNumNumbered = 6;  // C1..C6; CS, CE, CJ follow
extern const char* const kCycleLabels[kNumCycles];

constexpr int kOpStopped = 6;
constexpr int kOpStandby = 7;
constexpr int kOpMisting = 9;

struct Cycle {
  int act = 0, mode = 0, start_hour = 0, start_minute = 0, days_bitmap = 0,
      duration_seconds = 0, end_hour = 0, end_minute = 0, work_seconds = 0, pause_minutes = 0;
};

struct Settings {
  bool valid = false;  // true only when all 9 cycles were parsed
  int machine_type = 1;
  int progcic_type = 0;
  std::string firmware_minor;
  Cycle cycles[kNumCycles];
  std::string raw;
};

struct Dat {
  bool valid = false;
  int tick = -1, op_mode = -1, active_cycle = -1, cycle_state = -1;
  int planned = 0, elapsed = 0, stop_today = -1;
  int dev_year = 0, dev_month = 0, dev_day = 0, dev_hour = 0, dev_minute = 0, dev_second = 0;
  // cycle_state 1 = phase-1 spray, 2 = phase-2 spray (reported with op 6)
  bool pumpRunning() const { return cycle_state == 1 || cycle_state == 2; }
  bool spraying() const { return op_mode == kOpMisting || pumpRunning(); }
};

enum class Action { None, Mist, Pulse, Stop, StopDay, TestPump, TestLed, TestBuzzer };

std::vector<std::string> tokenize(const std::string& text);
bool parseGImp(const std::string& text, Settings& out);
bool parseGDat(const std::string& text, Dat& out);
std::string buildPDat(Action action);
std::string buildPImpHandshake(const LocalDateTime& now);
std::string buildPImpSettings(const Settings& s, const LocalDateTime& now);
long cicliplus(const Settings& s);
Settings withAllCyclesActive(const Settings& s, bool active);
bool anyCycleActive(const Settings& s);
std::string opModeName(int op);
std::string activeCycleName(int id);

}  // namespace z
```

- [ ] **Step 5: Write `lib/zcore/src/zprotocol.cpp`**

```cpp
#include "zprotocol.h"

#include <cstdlib>
#include <sstream>

namespace z {

const char* const kCycleLabels[kNumCycles] = {"C1", "C2", "C3", "C4", "C5", "C6", "CS", "CE", "CJ"};

static const char* const kPassword = "freezanz";
static const char* const kLang = "en-US";

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

static bool toInt(const std::vector<std::string>& t, size_t i, int& out) {
  if (i >= t.size()) return false;
  const char* s = t[i].c_str();
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = int(v);
  return true;
}

static int intAt(const std::vector<std::string>& t, size_t i, int fallback) {
  int v;
  return toInt(t, i, v) ? v : fallback;
}

static int labelIndex(const std::string& label) {
  for (int i = 0; i < kNumCycles; ++i)
    if (label == kCycleLabels[i]) return i;
  return -1;
}

bool parseGImp(const std::string& text, Settings& out) {
  auto t = tokenize(text);
  if (t.empty() || t[0] != "G_imp") return false;
  Settings s;
  s.raw = text;
  s.machine_type = intAt(t, 1, 1);
  s.progcic_type = intAt(t, 3, 0);
  s.firmware_minor = t.size() > 5 ? t[5] : "";
  bool seen[kNumCycles] = {};
  size_t i = 7;
  while (i < t.size()) {
    int idx = labelIndex(t[i]);
    if (idx < 0) break;
    bool numbered = idx < kNumNumbered;
    size_t need = numbered ? 10 : 8;
    if (i + need >= t.size()) break;
    int f[10] = {};
    for (size_t k = 0; k < need; ++k)
      if (!toInt(t, i + 1 + k, f[k])) return false;
    Cycle& c = s.cycles[idx];
    if (numbered) {
      c = Cycle{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]};
    } else {
      c = Cycle{f[0], f[1], 0, 0, f[2], f[3], f[4], f[5], f[6], f[7]};
    }
    seen[idx] = true;
    i += 1 + need;
  }
  s.valid = true;
  for (bool b : seen) s.valid = s.valid && b;
  out = s;
  return true;
}

bool parseGDat(const std::string& text, Dat& out) {
  auto t = tokenize(text);
  if (t.empty() || t[0] != "G_dat" || t.size() < 53) return false;
  Dat d;
  d.valid = true;
  d.tick = intAt(t, 1, -1);
  d.stop_today = intAt(t, 21, -1);
  d.dev_day = intAt(t, 27, 0);
  d.dev_month = intAt(t, 28, 0);
  d.dev_year = intAt(t, 29, 0);
  d.dev_hour = intAt(t, 30, 0);
  d.dev_minute = intAt(t, 31, 0);
  d.dev_second = intAt(t, 32, 0);
  d.op_mode = intAt(t, 38, -1);
  d.active_cycle = intAt(t, 44, -1);
  d.cycle_state = intAt(t, 45, -1);
  d.planned = intAt(t, 48, 0);
  d.elapsed = intAt(t, 52, 0);
  out = d;
  return true;
}

std::string buildPDat(Action a) {
  int flags[10] = {};
  switch (a) {
    case Action::Mist: flags[0] = 1; break;
    case Action::Pulse: flags[1] = 1; break;
    case Action::Stop: flags[2] = 1; break;
    case Action::StopDay: flags[3] = 1; break;
    case Action::TestPump: flags[4] = 1; break;
    case Action::TestLed: flags[5] = 1; break;
    case Action::TestBuzzer: flags[6] = 1; break;
    case Action::None: break;
  }
  std::string s = "P_dat;1920;1080;";
  for (int f : flags) {
    s += std::to_string(f);
    s += ';';
  }
  s += "1;0;0;";  // page_open, snd_test_send, snd_set_send
  return s;
}

static std::string dateHead(const LocalDateTime& n) {
  int chk = n.day + n.month + n.year + n.hour + n.minute + n.second;
  return std::to_string(n.weekday) + ";" + std::to_string(n.day) + ";" + std::to_string(n.month) + ";" +
         std::to_string(n.year) + ";" + std::to_string(n.hour) + ";" + std::to_string(n.minute) + ";" +
         std::to_string(n.second) + ";" + kLang + ";" + std::to_string(chk) + ";";
}

std::string buildPImpHandshake(const LocalDateTime& n) { return "P_imp;" + dateHead(n) + "A;0;"; }

std::string buildPImpSettings(const Settings& s, const LocalDateTime& n) {
  std::string out = "P_imp;" + dateHead(n) + "B;" + std::to_string(s.progcic_type) + ";";
  for (int i = 0; i < kNumCycles; ++i) {
    const Cycle& c = s.cycles[i];
    // Label and act are joined with a space, the rest with semicolons (device web UI post_impo()).
    out += std::string(kCycleLabels[i]) + " " + std::to_string(c.act) + ";";
    std::vector<int> f;
    if (i < kNumNumbered)
      f = {c.mode, c.start_hour, c.start_minute, c.days_bitmap, c.duration_seconds,
           c.end_hour, c.end_minute, c.work_seconds, c.pause_minutes};
    else
      f = {c.mode, c.days_bitmap, c.duration_seconds, c.end_hour, c.end_minute, c.work_seconds, c.pause_minutes};
    for (int v : f) out += std::to_string(v) + ";";
  }
  int machine = s.machine_type == 0 ? 1 : s.machine_type;
  // vbatcalib, cicliplus, change_machinetype, machine_type, psw_changed, psw, lingua, captive, chan, secure_save
  out += "0;" + std::to_string(cicliplus(s)) + ";0;" + std::to_string(machine) + ";0;" + kPassword + ";0;0;0;1;";
  return out;
}

long cicliplus(const Settings& s) {
  auto en = [&](int idx) { return s.cycles[idx].act ? 1L : 0L; };
  // C4, C5, C6, CS, CE, CJ
  return 1000000L * en(3) + 100000L * en(4) + 10000L * en(5) + 1000L * en(6) + 100L * en(7) + 10L * en(8);
}

Settings withAllCyclesActive(const Settings& s, bool active) {
  Settings out = s;
  for (auto& c : out.cycles) c.act = active ? 1 : 0;
  return out;
}

bool anyCycleActive(const Settings& s) {
  for (const auto& c : s.cycles)
    if (c.act) return true;
  return false;
}

std::string opModeName(int op) {
  switch (op) {
    case kOpStopped: return "Stopped";
    case kOpStandby: return "Standby";
    case kOpMisting: return "Misting";
    default: return "Unknown(" + std::to_string(op) + ")";
  }
}

std::string activeCycleName(int id) {
  if (id == 0) return "None";
  if (id == 12) return "Manual";
  if (id >= 1 && id <= 9) return "Cycle" + std::to_string(id);
  return "Unknown(" + std::to_string(id) + ")";
}

}  // namespace z
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `pio test -e native -f test_protocol`
Expected: 14 tests, all PASSED.

- [ ] **Step 7: Commit**

```bash
git add firmware && git commit -m "firmware: port Zhalt protocol to C++ with tests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Conversation state machine (`zconversation`)

One WebSocket session with the unit, with no I/O. The glue feeds it events (connected, frame, tick) and sends whatever it queues.

**Files:**
- Create: `firmware/lib/zcore/src/zconversation.h`, `zconversation.cpp`
- Test: `firmware/test/test_conversation/test_main.cpp`

**Interfaces:**
- Consumes: everything from Task 1.
- Produces:
  - `enum class z::Goal {Refresh, Mist, Pulse, Stop, TestPump, TestLed, TestBuzzer, Scheduler}`
  - `class z::Conversation` with:
    - `explicit Conversation(Goal)`
    - `setMistDurationMs(uint32_t)`
    - `setSchedulerTarget(bool on, const Settings* restoreFrom)`
    - `requestStop()`
    - `onConnected(uint32_t)`
    - `onFrame(const std::string&, uint32_t, const LocalDateTime&)`
    - `onTick(uint32_t, const LocalDateTime&)`
    - `std::vector<std::string> takeOutgoing()`
    - `Status status()` (`Running`/`Done`/`Failed`)
    - `const std::string& error()`
    - `bool started()`
    - `const Settings& settings()`, `const Dat& lastDat()`, `bool haveDat()`
  - Error strings: `no_greeting`, `no_status`, `not_started`, `stop_not_confirmed`, `settings_not_confirmed`, `no_cached_settings`, `no_settings`.

Behaviour rules:
- After `CiaO`, send the handshake.
- Wait up to 4 s for `G_imp`, sending a `P_dat` keepalive every 1.5 s, then act.
- **Mist / Pulse:** started when a `G_dat` shows `spraying()` within 30 s.
  - Pulse is Done at that point.
  - Mist then runs until its duration or `requestStop()`, then stops. If the unit goes idle for 2 `G_dat`s by itself, the mist is Done without a Stop.
- **Stop:** Done after 2 consecutive non-spraying `G_dat`s. The stop flag is re-sent every 10 s, and the stop fails after 25 s.
- **Scheduler:** send the B-form `P_imp`. Done when a later `G_imp` shows `anyCycleActive == target`. Re-handshake once after 3 s (forces a fresh `G_imp`). Fail after 15 s.
- **Refresh:** Done on the first `G_dat` after acting.
- **Tests (pump/LED/buzzer):** Done after 2 `G_dat`s after the flag.

- [ ] **Step 1: Write the failing test `test/test_conversation/test_main.cpp`**

```cpp
#include <unity.h>
#include "../fixtures.h"
#include "zconversation.h"

using namespace z;
using St = Conversation::Status;

void setUp() {}
void tearDown() {}

static const LocalDateTime kClock = toLocal(1778129712);  // 2026-05-07 08:55:12 Dubai
static const std::string kHandshake = "P_imp;4;7;5;2026;8;55;12;en-US;2113;A;0;";
static const std::string kPollNone = "P_dat;1920;1080;0;0;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollMist = "P_dat;1920;1080;1;0;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollPulse = "P_dat;1920;1080;0;1;0;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollStop = "P_dat;1920;1080;0;0;1;0;0;0;0;0;0;0;1;0;0;";
static const std::string kPollPump = "P_dat;1920;1080;0;0;0;0;1;0;0;0;0;0;1;0;0;";

static bool has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

// Connect at t=0, greeting at t=10, G_imp at t=110 (enters the act phase at 110).
static std::vector<std::string> open(Conversation& c) {
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onFrame(kGImp, 110, kClock);
  return c.takeOutgoing();
}

void test_refresh_happy_path() {
  Conversation c(Goal::Refresh);
  auto out = open(c);
  TEST_ASSERT_EQUAL_STRING(kHandshake.c_str(), out.at(0).c_str());
  TEST_ASSERT_TRUE(has(out, kPollNone));
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 300, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_TRUE(c.settings().valid);
  TEST_ASSERT_EQUAL(7, c.lastDat().op_mode);
}

void test_greeting_timeout() {
  Conversation c(Goal::Refresh);
  c.onConnected(0);
  c.onTick(7999, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onTick(8000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("no_greeting", c.error().c_str());
}

void test_no_g_imp_still_acts_after_wait() {
  Conversation c(Goal::Refresh);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onTick(1510, kClock);
  auto ka = c.takeOutgoing();
  TEST_ASSERT_TRUE(has(ka, kPollNone));  // keepalive while waiting for G_imp
  c.onTick(4010, kClock);
  c.onFrame(kGDatStandby, 4200, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_FALSE(c.settings().valid);
}

void test_mist_timed_stop() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(15000);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollMist));
  c.onFrame(kGDatMisting, 2000, kClock);
  TEST_ASSERT_TRUE(c.started());
  c.onTick(16999, kClock);
  TEST_ASSERT_FALSE(has(c.takeOutgoing(), kPollStop));
  c.onTick(17000, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
  c.onFrame(kGDatStandby, 18000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 19500, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_mist_unit_ends_by_itself() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(70000);
  open(c);
  c.onFrame(kGDatMisting, 2000, kClock);
  c.onFrame(kGDatStandby, 3500, kClock);
  c.onFrame(kGDatStandby, 5000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_FALSE(has(c.takeOutgoing(), kPollStop));
}

void test_mist_request_stop_early() {
  Conversation c(Goal::Mist);
  c.setMistDurationMs(70000);
  open(c);
  c.onFrame(kGDatMisting, 2000, kClock);
  c.requestStop();
  c.onTick(2100, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
}

void test_mist_request_stop_before_confirmed_start() {
  Conversation c(Goal::Mist);
  open(c);
  c.requestStop();
  c.onTick(200, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
}

void test_pulse_done_when_started() {
  Conversation c(Goal::Pulse);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollPulse));
  c.onFrame(kGDatMisting, 16000, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
  TEST_ASSERT_TRUE(c.started());
}

void test_pulse_not_started() {
  Conversation c(Goal::Pulse);
  open(c);
  c.onFrame(kGDatStandby, 1600, kClock);
  c.onTick(30109, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onTick(30110, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("not_started", c.error().c_str());
}

void test_stop_confirms_after_two_idle() {
  Conversation c(Goal::Stop);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollStop));
  c.onFrame(kGDatJustStopped, 1600, kClock);  // phase-2 spray still running
  c.onFrame(kGDatStandby, 3100, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 4600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_stop_resends_then_fails() {
  Conversation c(Goal::Stop);
  open(c);  // stop sent at 110
  c.takeOutgoing();
  c.onTick(10110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kPollStop));
  c.onTick(25110, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("stop_not_confirmed", c.error().c_str());
}

void test_scheduler_off() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(false, nullptr);
  auto out = open(c);
  Settings s;
  parseGImp(kGImp, s);
  TEST_ASSERT_TRUE(has(out, buildPImpSettings(withAllCyclesActive(s, false), kClock)));
  c.onFrame(kGImp, 1000, kClock);  // still on: not confirmed
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGImpAllOff, 1610, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_scheduler_rehandshakes_after_3s() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(false, nullptr);
  open(c);
  c.onTick(3110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), kHandshake));
}

void test_scheduler_on_restores_cached() {
  Settings cached;
  parseGImp(kGImp, cached);
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(true, &cached);
  c.onConnected(0);
  c.onFrame("CiaO", 10, kClock);
  c.onFrame(kGImpAllOff, 110, kClock);
  TEST_ASSERT_TRUE(has(c.takeOutgoing(), buildPImpSettings(cached, kClock)));
  c.onFrame(kGImp, 1600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

void test_scheduler_on_without_cache_fails() {
  Conversation c(Goal::Scheduler);
  c.setSchedulerTarget(true, nullptr);
  open(c);
  TEST_ASSERT_TRUE(c.status() == St::Failed);
  TEST_ASSERT_EQUAL_STRING("no_cached_settings", c.error().c_str());
}

void test_test_pump() {
  Conversation c(Goal::TestPump);
  auto out = open(c);
  TEST_ASSERT_TRUE(has(out, kPollPump));
  c.onFrame(kGDatStandby, 1600, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Running);
  c.onFrame(kGDatStandby, 3100, kClock);
  TEST_ASSERT_TRUE(c.status() == St::Done);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_refresh_happy_path);
  RUN_TEST(test_greeting_timeout);
  RUN_TEST(test_no_g_imp_still_acts_after_wait);
  RUN_TEST(test_mist_timed_stop);
  RUN_TEST(test_mist_unit_ends_by_itself);
  RUN_TEST(test_mist_request_stop_early);
  RUN_TEST(test_mist_request_stop_before_confirmed_start);
  RUN_TEST(test_pulse_done_when_started);
  RUN_TEST(test_pulse_not_started);
  RUN_TEST(test_stop_confirms_after_two_idle);
  RUN_TEST(test_stop_resends_then_fails);
  RUN_TEST(test_scheduler_off);
  RUN_TEST(test_scheduler_rehandshakes_after_3s);
  RUN_TEST(test_scheduler_on_restores_cached);
  RUN_TEST(test_scheduler_on_without_cache_fails);
  RUN_TEST(test_test_pump);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_conversation`
Expected: compile error, `zconversation.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/zconversation.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "zprotocol.h"

namespace z {

enum class Goal { Refresh, Mist, Pulse, Stop, TestPump, TestLed, TestBuzzer, Scheduler };

// One WebSocket session with the unit, as a pure state machine.
// Feed it events; send whatever takeOutgoing() returns, in order.
class Conversation {
 public:
  enum class Status { Running, Done, Failed };

  static constexpr uint32_t kPollMs = 1500;
  static constexpr uint32_t kGreetingTimeoutMs = 8000;
  static constexpr uint32_t kImpWaitMs = 4000;
  static constexpr uint32_t kStartVerifyMs = 30000;
  static constexpr uint32_t kStopVerifyMs = 25000;
  static constexpr uint32_t kStopResendMs = 10000;
  static constexpr uint32_t kSettingsVerifyMs = 15000;
  static constexpr uint32_t kRehandshakeMs = 3000;
  static constexpr int kIdleDatsForStop = 2;

  explicit Conversation(Goal goal);
  void setMistDurationMs(uint32_t ms);
  void setSchedulerTarget(bool on, const Settings* restoreFrom);
  void requestStop();

  void onConnected(uint32_t nowMs);
  void onFrame(const std::string& text, uint32_t nowMs, const LocalDateTime& clock);
  void onTick(uint32_t nowMs, const LocalDateTime& clock);
  std::vector<std::string> takeOutgoing();

  Status status() const { return status_; }
  const std::string& error() const { return error_; }
  bool started() const { return started_; }
  const Settings& settings() const { return settings_; }
  const Dat& lastDat() const { return dat_; }
  bool haveDat() const { return dat_.valid; }

 private:
  enum class Phase { WaitConnect, WaitGreeting, WaitImp, Act, VerifyStart, Running, VerifyStop, VerifySettings, Finished };

  void fail(const std::string& why);
  void finish();
  void enterAct(uint32_t nowMs, const LocalDateTime& clock);
  void beginStop(uint32_t nowMs);
  void poll(uint32_t nowMs, Action a);
  void onDat(const Dat& d, uint32_t nowMs);
  Action goalAction() const;

  Goal goal_;
  Phase phase_ = Phase::WaitConnect;
  Status status_ = Status::Running;
  std::string error_;
  std::vector<std::string> out_;
  Settings settings_;
  Dat dat_;
  uint32_t phaseStartMs_ = 0, lastPollMs_ = 0, sprayStartMs_ = 0, lastStopSendMs_ = 0;
  uint32_t mistMs_ = 70000;
  bool stopRequested_ = false, started_ = false, schedOn_ = false, rehandshook_ = false;
  const Settings* restoreFrom_ = nullptr;
  int idleDats_ = 0, datsSinceAct_ = 0;
};

}  // namespace z
```

- [ ] **Step 4: Write `lib/zcore/src/zconversation.cpp`**

```cpp
#include "zconversation.h"

namespace z {

Conversation::Conversation(Goal goal) : goal_(goal) {}

void Conversation::setMistDurationMs(uint32_t ms) { mistMs_ = ms; }

void Conversation::setSchedulerTarget(bool on, const Settings* restoreFrom) {
  schedOn_ = on;
  restoreFrom_ = restoreFrom;
}

void Conversation::requestStop() { stopRequested_ = true; }

std::vector<std::string> Conversation::takeOutgoing() {
  std::vector<std::string> o;
  o.swap(out_);
  return o;
}

void Conversation::onConnected(uint32_t nowMs) {
  if (phase_ != Phase::WaitConnect) return;
  phase_ = Phase::WaitGreeting;
  phaseStartMs_ = nowMs;
}

void Conversation::fail(const std::string& why) {
  if (status_ != Status::Running) return;
  status_ = Status::Failed;
  error_ = why;
  phase_ = Phase::Finished;
}

void Conversation::finish() {
  if (status_ != Status::Running) return;
  status_ = Status::Done;
  phase_ = Phase::Finished;
}

void Conversation::poll(uint32_t nowMs, Action a) {
  out_.push_back(buildPDat(a));
  lastPollMs_ = nowMs;
}

Action Conversation::goalAction() const {
  switch (goal_) {
    case Goal::Mist: return Action::Mist;
    case Goal::Pulse: return Action::Pulse;
    case Goal::Stop: return Action::Stop;
    case Goal::TestPump: return Action::TestPump;
    case Goal::TestLed: return Action::TestLed;
    case Goal::TestBuzzer: return Action::TestBuzzer;
    default: return Action::None;
  }
}

void Conversation::beginStop(uint32_t nowMs) {
  poll(nowMs, Action::Stop);
  lastStopSendMs_ = nowMs;
  phase_ = Phase::VerifyStop;
  phaseStartMs_ = nowMs;
  idleDats_ = 0;
}

void Conversation::enterAct(uint32_t nowMs, const LocalDateTime& clock) {
  phaseStartMs_ = nowMs;
  switch (goal_) {
    case Goal::Refresh:
      phase_ = Phase::Act;
      poll(nowMs, Action::None);
      return;
    case Goal::TestPump:
    case Goal::TestLed:
    case Goal::TestBuzzer:
      phase_ = Phase::Act;
      datsSinceAct_ = 0;
      poll(nowMs, goalAction());
      return;
    case Goal::Stop:
      beginStop(nowMs);
      return;
    case Goal::Mist:
    case Goal::Pulse:
      phase_ = Phase::VerifyStart;
      poll(nowMs, goalAction());
      return;
    case Goal::Scheduler: {
      if (schedOn_ && restoreFrom_ == nullptr) {
        fail("no_cached_settings");
        return;
      }
      if (!schedOn_ && !settings_.valid) {
        fail("no_settings");
        return;
      }
      Settings target = schedOn_ ? *restoreFrom_ : withAllCyclesActive(settings_, false);
      out_.push_back(buildPImpSettings(target, clock));
      lastPollMs_ = nowMs;
      rehandshook_ = false;
      phase_ = Phase::VerifySettings;
      return;
    }
  }
}

void Conversation::onFrame(const std::string& text, uint32_t nowMs, const LocalDateTime& clock) {
  if (status_ != Status::Running) return;
  if (text.compare(0, 4, kGreeting) == 0) {
    if (phase_ == Phase::WaitGreeting) {
      out_.push_back(buildPImpHandshake(clock));
      phase_ = Phase::WaitImp;
      phaseStartMs_ = nowMs;
      lastPollMs_ = nowMs;
    }
    return;
  }
  if (text.compare(0, 5, "G_imp") == 0) {
    Settings s;
    if (!parseGImp(text, s)) return;
    if (s.valid) settings_ = s;
    if (phase_ == Phase::WaitImp) {
      enterAct(nowMs, clock);
    } else if (phase_ == Phase::VerifySettings && s.valid && anyCycleActive(s) == schedOn_) {
      finish();
    }
    return;
  }
  if (text.compare(0, 5, "G_dat") == 0) {
    Dat d;
    if (parseGDat(text, d)) onDat(d, nowMs);
  }
}

void Conversation::onDat(const Dat& d, uint32_t nowMs) {
  dat_ = d;
  switch (phase_) {
    case Phase::Act:
      if (goal_ == Goal::Refresh) {
        finish();
        return;
      }
      if (++datsSinceAct_ >= 2) finish();
      return;
    case Phase::VerifyStart:
      if (d.spraying()) {
        started_ = true;
        sprayStartMs_ = nowMs;
        if (goal_ == Goal::Pulse) {
          finish();
          return;
        }
        phase_ = Phase::Running;
        idleDats_ = 0;
      }
      return;
    case Phase::Running:
    case Phase::VerifyStop:
      idleDats_ = d.spraying() ? 0 : idleDats_ + 1;
      if (idleDats_ >= kIdleDatsForStop) finish();
      return;
    default:
      return;
  }
}

void Conversation::onTick(uint32_t nowMs, const LocalDateTime& clock) {
  if (status_ != Status::Running) return;
  uint32_t inPhase = nowMs - phaseStartMs_;
  bool pollDue = nowMs - lastPollMs_ >= kPollMs;
  switch (phase_) {
    case Phase::WaitConnect:
    case Phase::Finished:
      return;
    case Phase::WaitGreeting:
      if (inPhase >= kGreetingTimeoutMs) fail("no_greeting");
      return;
    case Phase::WaitImp:
      if (inPhase >= kImpWaitMs) {
        enterAct(nowMs, clock);
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::Act:
      if (inPhase >= kStartVerifyMs) {
        fail("no_status");
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::VerifyStart:
      if (stopRequested_) {
        beginStop(nowMs);
        return;
      }
      if (inPhase >= kStartVerifyMs) {
        fail("not_started");
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::Running:
      if (stopRequested_ || nowMs - sprayStartMs_ >= mistMs_) {
        beginStop(nowMs);
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::VerifyStop:
      if (inPhase >= kStopVerifyMs) {
        fail("stop_not_confirmed");
        return;
      }
      if (nowMs - lastStopSendMs_ >= kStopResendMs) {
        poll(nowMs, Action::Stop);
        lastStopSendMs_ = nowMs;
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
    case Phase::VerifySettings:
      if (inPhase >= kSettingsVerifyMs) {
        fail("settings_not_confirmed");
        return;
      }
      if (!rehandshook_ && inPhase >= kRehandshakeMs) {
        out_.push_back(buildPImpHandshake(clock));
        rehandshook_ = true;
        return;
      }
      if (pollDue) poll(nowMs, Action::None);
      return;
  }
}

}  // namespace z
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e native -f test_conversation`
Expected: 16 tests, all PASSED.

- [ ] **Step 6: Commit**

```bash
git add firmware && git commit -m "firmware: conversation state machine for one unit session

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Config and command parsing (`zconfig`)

**Files:**
- Create: `firmware/lib/zcore/src/zconfig.h`, `zconfig.cpp`
- Test: `firmware/test/test_config/test_main.cpp`

**Interfaces:**
- Produces:
  - `z::Window{startMin,endMin}` with `enabled()` and `contains(int minuteOfDay)` (end exclusive, may cross midnight)
  - `z::kNumWindows = 3`
  - `z::Config{mistSeconds=70, resumeMinMinutes=10, windows[3]}`
  - `z::Config z::defaultConfig()`
  - `bool z::parseHHMM(const std::string&, int& minutes)`
  - `bool z::parseConfig(const std::string& json, Config& inOut, std::string& err)`: starts from `inOut`, overrides the keys present, and leaves `inOut` untouched on error
  - `std::string z::configToJson(const Config&)`
  - `enum class z::CmdType {Invalid, Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Reboot}`
  - `z::Command{type, seconds=0, minutes=0, on=false}`
  - `bool z::parseCommand(const std::string&, Command&, std::string& err)`

`zhalt/config` JSON shape (HA publishes it):
```json
{"mist_seconds": 70, "resume_min_minutes": 10,
 "blackouts": [{"start": "05:55:00", "end": "06:15:00"}, {"start": "07:00", "end": "07:20"}, {"start": "17:30", "end": "18:30"}]}
```

- [ ] **Step 1: Write the failing test `test/test_config/test_main.cpp`**

```cpp
#include <unity.h>
#include "zconfig.h"

using namespace z;

void setUp() {}
void tearDown() {}

void test_defaults() {
  Config c = defaultConfig();
  TEST_ASSERT_EQUAL(70, c.mistSeconds);
  TEST_ASSERT_EQUAL(10, c.resumeMinMinutes);
  TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
  TEST_ASSERT_EQUAL(375, c.windows[0].endMin);
  TEST_ASSERT_EQUAL(420, c.windows[1].startMin);
  TEST_ASSERT_EQUAL(440, c.windows[1].endMin);
  TEST_ASSERT_EQUAL(1050, c.windows[2].startMin);
  TEST_ASSERT_EQUAL(1110, c.windows[2].endMin);
}

void test_window_contains() {
  Window w{355, 375};
  TEST_ASSERT_FALSE(w.contains(354));
  TEST_ASSERT_TRUE(w.contains(355));
  TEST_ASSERT_TRUE(w.contains(374));
  TEST_ASSERT_FALSE(w.contains(375));
  Window night{1380, 60};  // 23:00-01:00
  TEST_ASSERT_TRUE(night.contains(1439));
  TEST_ASSERT_TRUE(night.contains(0));
  TEST_ASSERT_FALSE(night.contains(60));
  Window off;
  TEST_ASSERT_FALSE(off.enabled());
  TEST_ASSERT_FALSE(off.contains(0));
}

void test_parse_hhmm() {
  int m;
  TEST_ASSERT_TRUE(parseHHMM("05:55", m));
  TEST_ASSERT_EQUAL(355, m);
  TEST_ASSERT_TRUE(parseHHMM("05:55:00", m));
  TEST_ASSERT_EQUAL(355, m);
  TEST_ASSERT_TRUE(parseHHMM("7:00", m));
  TEST_ASSERT_EQUAL(420, m);
  TEST_ASSERT_FALSE(parseHHMM("25:00", m));
  TEST_ASSERT_FALSE(parseHHMM("05:60", m));
  TEST_ASSERT_FALSE(parseHHMM("05:55x", m));
  TEST_ASSERT_FALSE(parseHHMM("", m));
}

void test_parse_config_full() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(
      R"({"mist_seconds":60,"resume_min_minutes":15,"blackouts":[{"start":"05:50:00","end":"06:10:00"}]})", c, err));
  TEST_ASSERT_EQUAL(60, c.mistSeconds);
  TEST_ASSERT_EQUAL(15, c.resumeMinMinutes);
  TEST_ASSERT_EQUAL(350, c.windows[0].startMin);
  TEST_ASSERT_EQUAL(370, c.windows[0].endMin);
  TEST_ASSERT_FALSE(c.windows[1].enabled());  // a shorter list disables the rest
  TEST_ASSERT_FALSE(c.windows[2].enabled());
}

void test_parse_config_accepts_floats() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(R"({"mist_seconds":70.0,"resume_min_minutes":10.0})", c, err));
  TEST_ASSERT_EQUAL(70, c.mistSeconds);
}

void test_parse_config_partial_keeps_rest() {
  Config c = defaultConfig();
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(R"({"mist_seconds":45})", c, err));
  TEST_ASSERT_EQUAL(45, c.mistSeconds);
  TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
}

void test_parse_config_rejects_bad_input_and_keeps_previous() {
  const char* bad[] = {
      "not json",
      R"({"mist_seconds":200})",
      R"({"mist_seconds":"seventy"})",
      R"({"resume_min_minutes":-1})",
      R"({"blackouts":[{"start":"25:00","end":"06:00"}]})",
      R"({"blackouts":[{"start":"5:5","end":"06:00"}]})",
      R"({"blackouts":[{"start":"01:00","end":"02:00"},{"start":"03:00","end":"04:00"},{"start":"05:00","end":"06:00"},{"start":"07:00","end":"08:00"}]})",
      R"({"blackouts":"05:55-06:15"})",
  };
  for (const char* json : bad) {
    Config c = defaultConfig();
    std::string err;
    TEST_ASSERT_FALSE_MESSAGE(parseConfig(json, c, err), json);
    TEST_ASSERT_FALSE(err.empty());
    TEST_ASSERT_EQUAL(70, c.mistSeconds);
    TEST_ASSERT_EQUAL(355, c.windows[0].startMin);
  }
}

void test_config_json_round_trip() {
  Config c = defaultConfig();
  c.mistSeconds = 55;
  Config back;
  back.mistSeconds = 1;
  std::string err;
  TEST_ASSERT_TRUE(parseConfig(configToJson(c), back, err));
  TEST_ASSERT_EQUAL(55, back.mistSeconds);
  TEST_ASSERT_EQUAL(1050, back.windows[2].startMin);
  TEST_ASSERT_EQUAL(1110, back.windows[2].endMin);
}

void test_parse_commands() {
  Command c;
  std::string err;
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"mist","seconds":70})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Mist);
  TEST_ASSERT_EQUAL(70, c.seconds);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"mist"})", c, err));
  TEST_ASSERT_EQUAL(0, c.seconds);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"pulse","minutes":40})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Pulse);
  TEST_ASSERT_EQUAL(40, c.minutes);
  TEST_ASSERT_TRUE(parseCommand(R"({"action":"scheduler","on":false})", c, err));
  TEST_ASSERT_TRUE(c.type == CmdType::Scheduler);
  TEST_ASSERT_FALSE(c.on);
  const char* simple[] = {"stop", "refresh", "test_pump", "test_led", "test_buzzer", "test_join", "reboot"};
  CmdType types[] = {CmdType::Stop, CmdType::Refresh, CmdType::TestPump, CmdType::TestLed,
                     CmdType::TestBuzzer, CmdType::TestJoin, CmdType::Reboot};
  for (int i = 0; i < 7; ++i) {
    std::string json = std::string(R"({"action":")") + simple[i] + "\"}";
    TEST_ASSERT_TRUE(parseCommand(json, c, err));
    TEST_ASSERT_TRUE(c.type == types[i]);
  }
}

void test_parse_commands_rejects() {
  Command c;
  std::string err;
  TEST_ASSERT_FALSE(parseCommand("", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"explode"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"scheduler"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"({"action":"mist","seconds":"x"})", c, err));
  TEST_ASSERT_FALSE(parseCommand(R"(["mist"])", c, err));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_window_contains);
  RUN_TEST(test_parse_hhmm);
  RUN_TEST(test_parse_config_full);
  RUN_TEST(test_parse_config_accepts_floats);
  RUN_TEST(test_parse_config_partial_keeps_rest);
  RUN_TEST(test_parse_config_rejects_bad_input_and_keeps_previous);
  RUN_TEST(test_config_json_round_trip);
  RUN_TEST(test_parse_commands);
  RUN_TEST(test_parse_commands_rejects);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_config`
Expected: compile error, `zconfig.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/zconfig.h`**

```cpp
#pragma once
#include <string>

namespace z {

struct Window {
  int startMin = -1;  // minute of day, local
  int endMin = -1;    // exclusive; may be < startMin to cross midnight
  bool enabled() const { return startMin >= 0 && endMin >= 0 && startMin != endMin; }
  bool contains(int minuteOfDay) const;
};

constexpr int kNumWindows = 3;

struct Config {
  int mistSeconds = 70;
  int resumeMinMinutes = 10;
  Window windows[kNumWindows];
};

Config defaultConfig();
bool parseHHMM(const std::string& s, int& minutes);
bool parseConfig(const std::string& json, Config& inOut, std::string& err);
std::string configToJson(const Config& c);

enum class CmdType { Invalid, Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Reboot };

struct Command {
  CmdType type = CmdType::Invalid;
  int seconds = 0;  // mist; 0 = use config
  int minutes = 0;  // pulse; 0 = default
  bool on = false;  // scheduler
};

bool parseCommand(const std::string& json, Command& out, std::string& err);

}  // namespace z
```

- [ ] **Step 4: Write `lib/zcore/src/zconfig.cpp`**

```cpp
#include "zconfig.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace z {

bool Window::contains(int m) const {
  if (!enabled()) return false;
  if (startMin < endMin) return m >= startMin && m < endMin;
  return m >= startMin || m < endMin;
}

Config defaultConfig() {
  Config c;
  c.windows[0] = Window{5 * 60 + 55, 6 * 60 + 15};
  c.windows[1] = Window{7 * 60, 7 * 60 + 20};
  c.windows[2] = Window{17 * 60 + 30, 18 * 60 + 30};
  return c;
}

bool parseHHMM(const std::string& s, int& minutes) {
  int h = -1, m = -1, sec = 0, used = 0;
  if (std::sscanf(s.c_str(), "%2d:%2d:%2d%n", &h, &m, &sec, &used) == 3 && size_t(used) == s.size()) {
  } else if (std::sscanf(s.c_str(), "%2d:%2d%n", &h, &m, &used) == 2 && size_t(used) == s.size()) {
    sec = 0;
  } else {
    return false;
  }
  // "5:5" passes sscanf; require two-digit minutes
  size_t colon = s.find(':');
  if (colon == std::string::npos || s.size() < colon + 3) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
  minutes = h * 60 + m;
  return true;
}

static bool readInt(JsonVariantConst v, int& out) {
  if (!v.is<double>()) return false;
  out = int(std::lround(v.as<double>()));
  return true;
}

static std::string fmtHHMM(int minutes) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

bool parseConfig(const std::string& json, Config& inOut, std::string& err) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    err = "bad json";
    return false;
  }
  if (!doc.is<JsonObject>()) {
    err = "not an object";
    return false;
  }
  Config c = inOut;
  if (!doc["mist_seconds"].isNull()) {
    int v;
    if (!readInt(doc["mist_seconds"], v) || v < 5 || v > 120) {
      err = "mist_seconds must be 5-120";
      return false;
    }
    c.mistSeconds = v;
  }
  if (!doc["resume_min_minutes"].isNull()) {
    int v;
    if (!readInt(doc["resume_min_minutes"], v) || v < 0 || v > 90) {
      err = "resume_min_minutes must be 0-90";
      return false;
    }
    c.resumeMinMinutes = v;
  }
  if (!doc["blackouts"].isNull()) {
    if (!doc["blackouts"].is<JsonArray>()) {
      err = "blackouts must be an array";
      return false;
    }
    JsonArrayConst arr = doc["blackouts"].as<JsonArrayConst>();
    if (arr.size() > size_t(kNumWindows)) {
      err = "at most 3 blackouts";
      return false;
    }
    Window w[kNumWindows];
    size_t i = 0;
    for (JsonVariantConst item : arr) {
      int s, e;
      if (!parseHHMM(item["start"] | "", s) || !parseHHMM(item["end"] | "", e)) {
        err = "bad blackout time";
        return false;
      }
      w[i++] = Window{s, e};
    }
    for (int k = 0; k < kNumWindows; ++k) c.windows[k] = w[k];
  }
  inOut = c;
  return true;
}

std::string configToJson(const Config& c) {
  JsonDocument doc;
  doc["mist_seconds"] = c.mistSeconds;
  doc["resume_min_minutes"] = c.resumeMinMinutes;
  JsonArray arr = doc["blackouts"].to<JsonArray>();
  for (const auto& w : c.windows) {
    if (!w.enabled()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["start"] = fmtHHMM(w.startMin);
    o["end"] = fmtHHMM(w.endMin);
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseCommand(const std::string& json, Command& out, std::string& err) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    err = "bad json";
    return false;
  }
  static const struct {
    const char* name;
    CmdType type;
  } kMap[] = {{"mist", CmdType::Mist},           {"pulse", CmdType::Pulse},         {"stop", CmdType::Stop},
              {"refresh", CmdType::Refresh},     {"scheduler", CmdType::Scheduler}, {"test_pump", CmdType::TestPump},
              {"test_led", CmdType::TestLed},    {"test_buzzer", CmdType::TestBuzzer},
              {"test_join", CmdType::TestJoin},  {"reboot", CmdType::Reboot}};
  const char* action = doc["action"] | "";
  Command c;
  for (const auto& m : kMap)
    if (std::strcmp(action, m.name) == 0) c.type = m.type;
  if (c.type == CmdType::Invalid) {
    err = std::string("unknown action: ") + action;
    return false;
  }
  if (!doc["seconds"].isNull() && !readInt(doc["seconds"], c.seconds)) {
    err = "seconds must be a number";
    return false;
  }
  if (!doc["minutes"].isNull() && !readInt(doc["minutes"], c.minutes)) {
    err = "minutes must be a number";
    return false;
  }
  if (c.type == CmdType::Scheduler) {
    if (!doc["on"].is<bool>()) {
      err = "scheduler needs on: true/false";
      return false;
    }
    c.on = doc["on"].as<bool>();
  }
  out = c;
  return true;
}

}  // namespace z
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e native -f test_config`
Expected: 10 tests, all PASSED.

- [ ] **Step 6: Commit**

```bash
git add firmware && git commit -m "firmware: config and command parsing with validation

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Safety rules (`zsafety`)

**Files:**
- Create: `firmware/lib/zcore/src/zsafety.h`, `zsafety.cpp`
- Test: `firmware/test/test_safety/test_main.cpp`

**Interfaces:**
- Consumes: `Config`, `defaultConfig()` (Task 3); `localMinuteOfDay` (Task 1).
- Produces:
  - `z::SafetyState{pulseActive, pulseEndEpoch, paused, pausedRemainingSec, stopPending, stopInFlight, nextStopAt, stopAttempts, stopFailed}`
  - `enum class z::TickAction {None, Stop, ResumePulse}`
  - `z::TickResult{action, minutes}`
  - `enum class z::Gate {Ok, ClockNotSet, Blackout, PulseActive}`
  - `class z::Safety` with:
    - `setConfig`, `config`
    - `canStart(now, clockValid)`, `inBlackout(now)`
    - `clampMistSeconds(int)`, `clampPulseMinutes(int)`
    - `onPulseStarted(now, minutes)`, `requestStop(now)`, `onStopConfirmed()`, `onStopFailed(now)`
    - `tick(now, clockValid)`
    - `state()`, `restore(const SafetyState&)`, `serialize()`, `static deserialize(json, SafetyState&)`
  - Constants `kMistCapSec=120`, `kPulseMinMin=10`, `kPulseMaxMin=90`, `kPulseDefaultMin=40`, `kStopRetries=3`, `kStopRetryGapSec=20`.

Every stop goes through `Safety`: a user Stop calls `requestStop`, and `tick` returns `Stop` when one should be sent. The caller reports the outcome back with `onStopConfirmed`/`onStopFailed`.

- [ ] **Step 1: Write the failing test `test/test_safety/test_main.cpp`**

```cpp
#include <unity.h>
#include "zsafety.h"

using namespace z;

void setUp() {}
void tearDown() {}

// Epoch for 2026-10-15 hh:mm Dubai (05:55 = 1792029300)
static int64_t at(int h, int m) { return 1792029300LL + int64_t((h * 60 + m) - 355) * 60; }

void test_clamps() {
  Safety s;
  TEST_ASSERT_EQUAL(70, s.clampMistSeconds(0));
  TEST_ASSERT_EQUAL(15, s.clampMistSeconds(15));
  TEST_ASSERT_EQUAL(120, s.clampMistSeconds(200));
  TEST_ASSERT_EQUAL(40, s.clampPulseMinutes(0));
  TEST_ASSERT_EQUAL(10, s.clampPulseMinutes(5));
  TEST_ASSERT_EQUAL(90, s.clampPulseMinutes(200));
}

void test_gate_clock_and_blackout() {
  Safety s;
  TEST_ASSERT_TRUE(s.canStart(at(5, 50), false) == Gate::ClockNotSet);
  TEST_ASSERT_TRUE(s.canStart(at(5, 50), true) == Gate::Ok);
  TEST_ASSERT_TRUE(s.canStart(at(6, 0), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(6, 15), true) == Gate::Ok);
  TEST_ASSERT_TRUE(s.canStart(at(7, 10), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(17, 30), true) == Gate::Blackout);
  TEST_ASSERT_TRUE(s.canStart(at(18, 30), true) == Gate::Ok);
}

void test_gate_pulse_active() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.canStart(at(8, 10), true) == Gate::PulseActive);
}

void test_pulse_cap_stops_once() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.tick(at(8, 39), true).action == TickAction::None);
  TEST_ASSERT_TRUE(s.tick(at(8, 40), true).action == TickAction::Stop);
  TEST_ASSERT_TRUE(s.tick(at(8, 40) + 1, true).action == TickAction::None);  // in flight
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(8, 41), true).action == TickAction::None);
  TEST_ASSERT_FALSE(s.state().pulseActive);
  TEST_ASSERT_FALSE(s.state().stopPending);
}

void test_blackout_stops_then_resumes() {
  Safety s;
  s.onPulseStarted(at(5, 45), 40);  // would end 06:25
  TEST_ASSERT_TRUE(s.tick(at(5, 54), true).action == TickAction::None);
  TEST_ASSERT_TRUE(s.tick(at(5, 55), true).action == TickAction::Stop);
  TEST_ASSERT_TRUE(s.state().paused);
  TEST_ASSERT_EQUAL(30 * 60, (int)s.state().pausedRemainingSec);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(6, 10), true).action == TickAction::None);
  TickResult r = s.tick(at(6, 15), true);
  TEST_ASSERT_TRUE(r.action == TickAction::ResumePulse);
  TEST_ASSERT_EQUAL(30, r.minutes);
  TEST_ASSERT_TRUE(s.tick(at(6, 16), true).action == TickAction::None);
}

void test_blackout_no_resume_below_threshold() {
  Safety s;
  s.onPulseStarted(at(5, 50), 10);  // would end 06:00, 5 min left at 05:55
  TEST_ASSERT_TRUE(s.tick(at(5, 55), true).action == TickAction::Stop);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(6, 15), true).action == TickAction::None);
  TEST_ASSERT_FALSE(s.state().paused);
}

void test_blackout_resume_threshold_zero_always_resumes() {
  Safety s;
  Config c = defaultConfig();
  c.resumeMinMinutes = 0;
  s.setConfig(c);
  s.onPulseStarted(at(5, 50), 10);
  s.tick(at(5, 55), true);
  s.onStopConfirmed();
  TickResult r = s.tick(at(6, 15), true);
  TEST_ASSERT_TRUE(r.action == TickAction::ResumePulse);
  TEST_ASSERT_EQUAL(5, r.minutes);
}

void test_stop_retries_then_fails() {
  Safety s;
  int64_t t = at(9, 0);
  s.requestStop(t);
  TEST_ASSERT_TRUE(s.tick(t, true).action == TickAction::Stop);
  for (int retry = 1; retry <= 3; ++retry) {
    s.onStopFailed(t);
    TEST_ASSERT_TRUE(s.tick(t + 19, true).action == TickAction::None);
    t += 20;
    TEST_ASSERT_TRUE(s.tick(t, true).action == TickAction::Stop);
  }
  s.onStopFailed(t);
  TEST_ASSERT_TRUE(s.state().stopFailed);
  TEST_ASSERT_FALSE(s.state().stopPending);
  TEST_ASSERT_TRUE(s.tick(t + 100, true).action == TickAction::None);
}

void test_user_stop_cancels_pulse() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  s.requestStop(at(8, 5));
  TEST_ASSERT_TRUE(s.tick(at(8, 5), true).action == TickAction::Stop);
  s.onStopConfirmed();
  TEST_ASSERT_TRUE(s.tick(at(8, 40), true).action == TickAction::None);
}

void test_clock_invalid_defers_timers_but_not_stops() {
  Safety s;
  s.onPulseStarted(at(8, 0), 40);
  TEST_ASSERT_TRUE(s.tick(at(8, 41), false).action == TickAction::None);
  s.requestStop(at(8, 41));
  TEST_ASSERT_TRUE(s.tick(at(8, 41), false).action == TickAction::Stop);
}

void test_restore_after_reboot_still_stops() {
  Safety before;
  before.onPulseStarted(at(8, 0), 40);
  std::string saved = before.serialize();

  SafetyState st;
  TEST_ASSERT_TRUE(Safety::deserialize(saved, st));
  Safety after;
  after.restore(st);
  TEST_ASSERT_TRUE(after.state().pulseActive);
  TEST_ASSERT_TRUE(after.tick(at(8, 40), true).action == TickAction::Stop);
}

void test_restore_with_stop_in_flight_retries() {
  Safety before;
  before.requestStop(at(9, 0));
  before.tick(at(9, 0), true);  // stop in flight, then reboot
  SafetyState st;
  TEST_ASSERT_TRUE(Safety::deserialize(before.serialize(), st));
  Safety after;
  after.restore(st);
  TEST_ASSERT_TRUE(after.tick(at(9, 1), true).action == TickAction::Stop);
}

void test_deserialize_rejects_garbage() {
  SafetyState st;
  TEST_ASSERT_FALSE(Safety::deserialize("", st));
  TEST_ASSERT_FALSE(Safety::deserialize("nope", st));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clamps);
  RUN_TEST(test_gate_clock_and_blackout);
  RUN_TEST(test_gate_pulse_active);
  RUN_TEST(test_pulse_cap_stops_once);
  RUN_TEST(test_blackout_stops_then_resumes);
  RUN_TEST(test_blackout_no_resume_below_threshold);
  RUN_TEST(test_blackout_resume_threshold_zero_always_resumes);
  RUN_TEST(test_stop_retries_then_fails);
  RUN_TEST(test_user_stop_cancels_pulse);
  RUN_TEST(test_clock_invalid_defers_timers_but_not_stops);
  RUN_TEST(test_restore_after_reboot_still_stops);
  RUN_TEST(test_restore_with_stop_in_flight_retries);
  RUN_TEST(test_deserialize_rejects_garbage);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_safety`
Expected: compile error, `zsafety.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/zsafety.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>

#include "zconfig.h"

namespace z {

struct SafetyState {
  bool pulseActive = false;
  int64_t pulseEndEpoch = 0;
  bool paused = false;  // pulse stopped for a blackout; may resume when it ends
  int64_t pausedRemainingSec = 0;
  bool stopPending = false;
  bool stopInFlight = false;  // not persisted
  int64_t nextStopAt = 0;
  int stopAttempts = 0;
  bool stopFailed = false;
};

enum class TickAction { None, Stop, ResumePulse };
struct TickResult {
  TickAction action = TickAction::None;
  int minutes = 0;
};
enum class Gate { Ok, ClockNotSet, Blackout, PulseActive };

class Safety {
 public:
  static constexpr int kMistCapSec = 120;
  static constexpr int kPulseMinMin = 10;
  static constexpr int kPulseMaxMin = 90;
  static constexpr int kPulseDefaultMin = 40;
  static constexpr int kStopRetries = 3;
  static constexpr int kStopRetryGapSec = 20;

  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  Gate canStart(int64_t now, bool clockValid) const;
  bool inBlackout(int64_t now) const;
  int clampMistSeconds(int requested) const;
  int clampPulseMinutes(int requested) const;

  void onPulseStarted(int64_t now, int minutes);
  void requestStop(int64_t now);  // user/HA stop: cancels any session
  void onStopConfirmed();
  void onStopFailed(int64_t now);
  TickResult tick(int64_t now, bool clockValid);  // call about once a second

  const SafetyState& state() const { return st_; }
  void restore(const SafetyState& s);
  std::string serialize() const;
  static bool deserialize(const std::string& json, SafetyState& out);

 private:
  void beginStop(int64_t now);

  Config config_ = defaultConfig();
  SafetyState st_;
};

}  // namespace z
```

- [ ] **Step 4: Write `lib/zcore/src/zsafety.cpp`**

```cpp
#include "zsafety.h"

#include <ArduinoJson.h>

#include "ztime.h"

namespace z {

bool Safety::inBlackout(int64_t now) const {
  int m = localMinuteOfDay(now);
  for (const auto& w : config_.windows)
    if (w.contains(m)) return true;
  return false;
}

Gate Safety::canStart(int64_t now, bool clockValid) const {
  if (!clockValid) return Gate::ClockNotSet;
  if (inBlackout(now)) return Gate::Blackout;
  if (st_.pulseActive) return Gate::PulseActive;
  return Gate::Ok;
}

int Safety::clampMistSeconds(int requested) const {
  int s = requested > 0 ? requested : config_.mistSeconds;
  if (s < 1) s = 1;
  if (s > kMistCapSec) s = kMistCapSec;
  return s;
}

int Safety::clampPulseMinutes(int requested) const {
  int m = requested > 0 ? requested : kPulseDefaultMin;
  if (m < kPulseMinMin) m = kPulseMinMin;
  if (m > kPulseMaxMin) m = kPulseMaxMin;
  return m;
}

void Safety::onPulseStarted(int64_t now, int minutes) {
  st_.pulseActive = true;
  st_.pulseEndEpoch = now + int64_t(minutes) * 60;
  st_.paused = false;
  st_.pausedRemainingSec = 0;
  st_.stopFailed = false;
}

void Safety::beginStop(int64_t now) {
  st_.stopPending = true;
  st_.stopInFlight = false;
  st_.nextStopAt = now;
  st_.stopAttempts = 0;
  st_.stopFailed = false;
}

void Safety::requestStop(int64_t now) {
  st_.pulseActive = false;
  st_.paused = false;
  st_.pausedRemainingSec = 0;
  beginStop(now);
}

void Safety::onStopConfirmed() {
  st_.stopPending = false;
  st_.stopInFlight = false;
  st_.stopAttempts = 0;
}

void Safety::onStopFailed(int64_t now) {
  st_.stopInFlight = false;
  st_.stopAttempts++;
  if (st_.stopAttempts > kStopRetries) {
    st_.stopPending = false;
    st_.stopFailed = true;
    st_.pulseActive = false;
    st_.paused = false;
    st_.pausedRemainingSec = 0;
  } else {
    st_.nextStopAt = now + kStopRetryGapSec;
  }
}

TickResult Safety::tick(int64_t now, bool clockValid) {
  if (st_.stopPending) {
    if (!st_.stopInFlight && now >= st_.nextStopAt) {
      st_.stopInFlight = true;
      return {TickAction::Stop, 0};
    }
    return {};
  }
  if (!clockValid) return {};
  if (st_.pulseActive) {
    if (inBlackout(now)) {
      int64_t rem = st_.pulseEndEpoch - now;
      st_.pulseActive = false;
      st_.paused = rem > 0;
      st_.pausedRemainingSec = rem > 0 ? rem : 0;
      beginStop(now);
      st_.stopInFlight = true;
      return {TickAction::Stop, 0};
    }
    if (now >= st_.pulseEndEpoch) {
      st_.pulseActive = false;
      beginStop(now);
      st_.stopInFlight = true;
      return {TickAction::Stop, 0};
    }
    return {};
  }
  if (st_.paused && !inBlackout(now)) {
    int64_t rem = st_.pausedRemainingSec;
    st_.paused = false;
    st_.pausedRemainingSec = 0;
    if (rem > 0 && rem >= int64_t(config_.resumeMinMinutes) * 60)
      return {TickAction::ResumePulse, int((rem + 59) / 60)};
  }
  return {};
}

void Safety::restore(const SafetyState& s) {
  st_ = s;
  st_.stopInFlight = false;
  if (st_.stopPending) st_.nextStopAt = 0;  // retry straight away after a reboot
}

std::string Safety::serialize() const {
  JsonDocument d;
  d["pa"] = st_.pulseActive;
  d["pe"] = st_.pulseEndEpoch;
  d["p"] = st_.paused;
  d["pr"] = st_.pausedRemainingSec;
  d["sp"] = st_.stopPending;
  d["na"] = st_.nextStopAt;
  d["sa"] = st_.stopAttempts;
  d["sf"] = st_.stopFailed;
  std::string s;
  serializeJson(d, s);
  return s;
}

bool Safety::deserialize(const std::string& json, SafetyState& out) {
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonObject>()) return false;
  SafetyState s;
  s.pulseActive = d["pa"] | false;
  s.pulseEndEpoch = d["pe"] | int64_t(0);
  s.paused = d["p"] | false;
  s.pausedRemainingSec = d["pr"] | int64_t(0);
  s.stopPending = d["sp"] | false;
  s.nextStopAt = d["na"] | int64_t(0);
  s.stopAttempts = d["sa"] | 0;
  s.stopFailed = d["sf"] | false;
  out = s;
  return true;
}

}  // namespace z
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e native -f test_safety`
Expected: 13 tests, all PASSED.

- [ ] **Step 6: Commit**

```bash
git add firmware && git commit -m "firmware: board-side safety rules with persistence

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Job queue (`zjobs`)

**Files:**
- Create: `firmware/lib/zcore/src/zjobs.h`, `zjobs.cpp`
- Test: `firmware/test/test_jobs/test_main.cpp`

**Interfaces:**
- Produces:
  - `enum class z::JobType {Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Passthrough}`
  - `z::Job{type, seconds=0, minutes=0, on=false, resume=false}`
  - `class z::JobQueue{kMax=8; bool push(const Job&); bool pop(Job&); size_t size() const; bool contains(JobType) const;}`
- Not thread-safe; the glue holds `app.mu` around every call.

- [ ] **Step 1: Write the failing test `test/test_jobs/test_main.cpp`**

```cpp
#include <unity.h>
#include "zjobs.h"

using namespace z;

void setUp() {}
void tearDown() {}

static Job J(JobType t) {
  Job j;
  j.type = t;
  return j;
}

void test_fifo() {
  JobQueue q;
  q.push(J(JobType::Refresh));
  q.push(J(JobType::Mist));
  Job out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_TRUE(out.type == JobType::Refresh);
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_TRUE(out.type == JobType::Mist);
  TEST_ASSERT_FALSE(q.pop(out));
}

void test_stop_jumps_queue() {
  JobQueue q;
  q.push(J(JobType::Refresh));
  q.push(J(JobType::TestJoin));
  q.push(J(JobType::Stop));
  Job out;
  q.pop(out);
  TEST_ASSERT_TRUE(out.type == JobType::Stop);
}

void test_stop_drops_queued_starts() {
  JobQueue q;
  q.push(J(JobType::Mist));
  q.push(J(JobType::Refresh));
  q.push(J(JobType::Pulse));
  q.push(J(JobType::Stop));
  TEST_ASSERT_EQUAL(2, (int)q.size());
  TEST_ASSERT_FALSE(q.contains(JobType::Mist));
  TEST_ASSERT_FALSE(q.contains(JobType::Pulse));
  TEST_ASSERT_TRUE(q.contains(JobType::Refresh));
}

void test_duplicate_stop_collapses() {
  JobQueue q;
  q.push(J(JobType::Stop));
  q.push(J(JobType::Stop));
  TEST_ASSERT_EQUAL(1, (int)q.size());
}

void test_full_queue_rejects_but_stop_still_fits() {
  JobQueue q;
  for (size_t i = 0; i < JobQueue::kMax; ++i) TEST_ASSERT_TRUE(q.push(J(JobType::Refresh)));
  TEST_ASSERT_FALSE(q.push(J(JobType::Refresh)));
  TEST_ASSERT_TRUE(q.push(J(JobType::Stop)));
  Job out;
  q.pop(out);
  TEST_ASSERT_TRUE(out.type == JobType::Stop);
}

void test_job_fields_carried() {
  JobQueue q;
  Job j = J(JobType::Pulse);
  j.minutes = 30;
  j.resume = true;
  q.push(j);
  Job out;
  q.pop(out);
  TEST_ASSERT_EQUAL(30, out.minutes);
  TEST_ASSERT_TRUE(out.resume);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifo);
  RUN_TEST(test_stop_jumps_queue);
  RUN_TEST(test_stop_drops_queued_starts);
  RUN_TEST(test_duplicate_stop_collapses);
  RUN_TEST(test_full_queue_rejects_but_stop_still_fits);
  RUN_TEST(test_job_fields_carried);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_jobs`
Expected: compile error, `zjobs.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/zjobs.h` and `zjobs.cpp`**

```cpp
// zjobs.h
#pragma once
#include <cstddef>
#include <deque>

namespace z {

enum class JobType { Mist, Pulse, Stop, Refresh, Scheduler, TestPump, TestLed, TestBuzzer, TestJoin, Passthrough };

struct Job {
  JobType type = JobType::Refresh;
  int seconds = 0;
  int minutes = 0;
  bool on = false;
  bool resume = false;  // pulse restarted after a blackout: minutes are not clamped
};

// One worker drains this. Stop jumps the queue and cancels queued starts.
class JobQueue {
 public:
  static constexpr size_t kMax = 8;
  bool push(const Job& j);
  bool pop(Job& out);
  size_t size() const { return q_.size(); }
  bool contains(JobType t) const;

 private:
  std::deque<Job> q_;
};

}  // namespace z
```

```cpp
// zjobs.cpp
#include "zjobs.h"

namespace z {

bool JobQueue::contains(JobType t) const {
  for (const auto& j : q_)
    if (j.type == t) return true;
  return false;
}

bool JobQueue::push(const Job& j) {
  if (j.type == JobType::Stop) {
    for (auto it = q_.begin(); it != q_.end();) {
      if (it->type == JobType::Mist || it->type == JobType::Pulse)
        it = q_.erase(it);
      else
        ++it;
    }
    if (!contains(JobType::Stop)) q_.push_front(j);
    return true;
  }
  if (q_.size() >= kMax) return false;
  q_.push_back(j);
  return true;
}

bool JobQueue::pop(Job& out) {
  if (q_.empty()) return false;
  out = q_.front();
  q_.pop_front();
  return true;
}

}  // namespace z
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_jobs`
Expected: 6 tests, all PASSED.

- [ ] **Step 5: Commit**

```bash
git add firmware && git commit -m "firmware: job queue with stop priority

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: State JSON and HA discovery (`zpublish`)

**Files:**
- Create: `firmware/lib/zcore/src/zpublish.h`, `zpublish.cpp`
- Test: `firmware/test/test_publish/test_main.cpp`

**Interfaces:**
- Consumes:
  - `SafetyState` (Task 4)
  - `toLocal` (Task 1)
  - `opModeName`, `activeCycleName` (Task 1)
- Produces:
  - Topic constants `z::kTopicCmd`, `z::kTopicConfig`, `z::kTopicState`, `z::kTopicStatus`
  - `z::BridgeSnapshot` (fields below)
  - `std::string z::isoLocal(int64_t)`
  - `std::string z::deriveState(const SafetyState&, bool mistActive)`, which returns `stop_failed` / `stopping` / `misting` / `pulse_session` / `paused_blackout` / `idle`
  - `std::string z::stateJson(const BridgeSnapshot&)`
  - `struct z::DiscoveryMsg{topic, payload}`
  - `std::vector<DiscoveryMsg> z::discoveryMessages(const std::string& mac, const std::string& fw, const std::string& ip)`, which returns 30 messages

`zhalt/state` keys:
- `state`, `misting`, `session_end`, `last_mist`, `last_mist_kind`, `last_result`, `scheduler_on`
- `unit_state`, `active_cycle`, `unit_clock`, `unit_firmware`, `last_contact`
- `zhalt_reachable`, `rssi`, `join_ms`, `join_failures_24h`, `uptime_s`, `free_heap`, `reset_reason`, `fw`, `ip`, `passthrough`

Unknown values are JSON `null`, so HA templates render `None` and the entity state becomes unknown.

- [ ] **Step 1: Write the failing test `test/test_publish/test_main.cpp`**

```cpp
#include <ArduinoJson.h>
#include <unity.h>

#include <set>

#include "zpublish.h"

using namespace z;

void setUp() {}
void tearDown() {}

void test_iso_local() {
  TEST_ASSERT_EQUAL_STRING("2026-10-15T05:55:00+04:00", isoLocal(1792029300).c_str());
  TEST_ASSERT_EQUAL_STRING("", isoLocal(0).c_str());
}

void test_derive_state() {
  SafetyState s;
  TEST_ASSERT_EQUAL_STRING("idle", deriveState(s, false).c_str());
  TEST_ASSERT_EQUAL_STRING("misting", deriveState(s, true).c_str());
  s.pulseActive = true;
  TEST_ASSERT_EQUAL_STRING("pulse_session", deriveState(s, false).c_str());
  s.pulseActive = false;
  s.paused = true;
  TEST_ASSERT_EQUAL_STRING("paused_blackout", deriveState(s, false).c_str());
  s.stopPending = true;
  TEST_ASSERT_EQUAL_STRING("stopping", deriveState(s, false).c_str());
  s.stopFailed = true;
  TEST_ASSERT_EQUAL_STRING("stop_failed", deriveState(s, false).c_str());
}

void test_state_json_idle_has_nulls() {
  BridgeSnapshot s;
  JsonDocument d;
  deserializeJson(d, stateJson(s));
  TEST_ASSERT_EQUAL_STRING("idle", d["state"].as<const char*>());
  TEST_ASSERT_TRUE(d["session_end"].isNull());
  TEST_ASSERT_TRUE(d["last_mist"].isNull());
  TEST_ASSERT_TRUE(d["scheduler_on"].isNull());
  TEST_ASSERT_TRUE(d["unit_state"].isNull());
  TEST_ASSERT_TRUE(d["rssi"].isNull());
  TEST_ASSERT_FALSE(d["misting"].as<bool>());
}

void test_state_json_values() {
  BridgeSnapshot s;
  s.state = "pulse_session";
  s.misting = true;
  s.sessionEnd = 1792029300;
  s.lastMist = 1792029300;
  s.lastMistKind = "pulse";
  s.lastResult = "ok";
  s.schedulerOn = 0;
  s.unitValid = true;
  s.unitOp = 9;
  s.unitActiveCycle = 12;
  s.rssiKnown = true;
  s.rssi = -61;
  s.joinMs = 3200;
  JsonDocument d;
  deserializeJson(d, stateJson(s));
  TEST_ASSERT_EQUAL_STRING("2026-10-15T05:55:00+04:00", d["session_end"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("pulse", d["last_mist_kind"].as<const char*>());
  TEST_ASSERT_FALSE(d["scheduler_on"].as<bool>());
  TEST_ASSERT_FALSE(d["scheduler_on"].isNull());
  TEST_ASSERT_EQUAL_STRING("Misting", d["unit_state"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Manual", d["active_cycle"].as<const char*>());
  TEST_ASSERT_EQUAL(-61, d["rssi"].as<int>());
  TEST_ASSERT_EQUAL(3200, d["join_ms"].as<int>());
}

static JsonDocument findPayload(const std::vector<DiscoveryMsg>& msgs, const std::string& topic) {
  JsonDocument d;
  for (const auto& m : msgs)
    if (m.topic == topic) deserializeJson(d, m.payload);
  return d;
}

void test_discovery_count_and_unique_ids() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  TEST_ASSERT_EQUAL(30, (int)msgs.size());
  std::set<std::string> ids, topics;
  for (const auto& m : msgs) {
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, m.payload));
    ids.insert(d["unique_id"].as<std::string>());
    topics.insert(m.topic);
  }
  TEST_ASSERT_EQUAL(30, (int)ids.size());
  TEST_ASSERT_EQUAL(30, (int)topics.size());
}

void test_discovery_mist_button() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/button/zhalt_aabbcc/mist_now/config");
  TEST_ASSERT_EQUAL_STRING("button.zhalt_mqtt_mist_now", d["default_entity_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt/cmd", d["command_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"mist\"}", d["payload_press"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt/bridge/status", d["availability_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Zhalt Mosquito", d["device"]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("zhalt_bridge_aabbcc", d["device"]["via_device"].as<const char*>());
}

void test_discovery_online_sensor_has_no_availability() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/binary_sensor/zhalt_aabbcc/online/config");
  TEST_ASSERT_TRUE(d["availability_topic"].isNull());
  TEST_ASSERT_EQUAL_STRING("zhalt/bridge/status", d["state_topic"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("online", d["payload_on"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Zhalt Bridge", d["device"]["name"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.30/", d["device"]["configuration_url"].as<const char*>());
}

void test_discovery_scheduler_switch() {
  auto msgs = discoveryMessages("aabbcc", "0.1.0", "10.0.0.30");
  JsonDocument d = findPayload(msgs, "homeassistant/switch/zhalt_aabbcc/onboard_scheduler/config");
  TEST_ASSERT_EQUAL_STRING("switch.zhalt_mqtt_onboard_scheduler", d["default_entity_id"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"scheduler\",\"on\":false}", d["payload_off"].as<const char*>());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_iso_local);
  RUN_TEST(test_derive_state);
  RUN_TEST(test_state_json_idle_has_nulls);
  RUN_TEST(test_state_json_values);
  RUN_TEST(test_discovery_count_and_unique_ids);
  RUN_TEST(test_discovery_mist_button);
  RUN_TEST(test_discovery_online_sensor_has_no_availability);
  RUN_TEST(test_discovery_scheduler_switch);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `pio test -e native -f test_publish`
Expected: compile error, `zpublish.h: No such file or directory`.

- [ ] **Step 3: Write `lib/zcore/src/zpublish.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "zsafety.h"

namespace z {

constexpr const char* kTopicCmd = "zhalt/cmd";
constexpr const char* kTopicConfig = "zhalt/config";
constexpr const char* kTopicState = "zhalt/state";
constexpr const char* kTopicStatus = "zhalt/bridge/status";

struct BridgeSnapshot {
  std::string state = "idle";
  bool misting = false;
  int64_t sessionEnd = 0;
  int64_t lastMist = 0;
  std::string lastMistKind;
  std::string lastResult;
  int schedulerOn = -1;  // -1 unknown, 0 off, 1 on
  bool unitValid = false;
  int unitOp = -1;
  int unitActiveCycle = -1;
  std::string unitClock;
  std::string unitFirmware;
  int64_t lastContact = 0;
  bool zhaltReachable = false;
  bool rssiKnown = false;
  int rssi = 0;
  uint32_t joinMs = 0;
  int joinFailures24h = 0;
  uint32_t uptimeS = 0;
  uint32_t freeHeap = 0;
  std::string resetReason;
  std::string fw;
  std::string ip;
  bool passthrough = false;
};

struct DiscoveryMsg {
  std::string topic;
  std::string payload;
};

std::string isoLocal(int64_t epoch);
std::string deriveState(const SafetyState& s, bool mistActive);
std::string stateJson(const BridgeSnapshot& s);
std::vector<DiscoveryMsg> discoveryMessages(const std::string& mac, const std::string& fw, const std::string& ip);

}  // namespace z
```

- [ ] **Step 4: Write `lib/zcore/src/zpublish.cpp`**

```cpp
#include "zpublish.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "zprotocol.h"
#include "ztime.h"

namespace z {

std::string isoLocal(int64_t epoch) {
  if (epoch <= 0) return "";
  LocalDateTime t = toLocal(epoch);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d+04:00", t.year, t.month, t.day, t.hour, t.minute,
                t.second);
  return buf;
}

std::string deriveState(const SafetyState& s, bool mistActive) {
  if (s.stopFailed) return "stop_failed";
  if (s.stopPending) return "stopping";
  if (mistActive) return "misting";
  if (s.pulseActive) return "pulse_session";
  if (s.paused) return "paused_blackout";
  return "idle";
}

static void putTime(JsonDocument& d, const char* key, int64_t epoch) {
  if (epoch > 0)
    d[key] = isoLocal(epoch);
  else
    d[key] = nullptr;
}

static void putStr(JsonDocument& d, const char* key, const std::string& v) {
  if (v.empty())
    d[key] = nullptr;
  else
    d[key] = v;
}

std::string stateJson(const BridgeSnapshot& s) {
  JsonDocument d;
  d["state"] = s.state;
  d["misting"] = s.misting;
  putTime(d, "session_end", s.sessionEnd);
  putTime(d, "last_mist", s.lastMist);
  putStr(d, "last_mist_kind", s.lastMistKind);
  putStr(d, "last_result", s.lastResult);
  if (s.schedulerOn < 0)
    d["scheduler_on"] = nullptr;
  else
    d["scheduler_on"] = s.schedulerOn == 1;
  if (s.unitValid) {
    d["unit_state"] = opModeName(s.unitOp);
    d["active_cycle"] = activeCycleName(s.unitActiveCycle);
  } else {
    d["unit_state"] = nullptr;
    d["active_cycle"] = nullptr;
  }
  putStr(d, "unit_clock", s.unitClock);
  putStr(d, "unit_firmware", s.unitFirmware);
  putTime(d, "last_contact", s.lastContact);
  d["zhalt_reachable"] = s.zhaltReachable;
  if (s.rssiKnown)
    d["rssi"] = s.rssi;
  else
    d["rssi"] = nullptr;
  if (s.joinMs > 0)
    d["join_ms"] = s.joinMs;
  else
    d["join_ms"] = nullptr;
  d["join_failures_24h"] = s.joinFailures24h;
  d["uptime_s"] = s.uptimeS;
  d["free_heap"] = s.freeHeap;
  putStr(d, "reset_reason", s.resetReason);
  putStr(d, "fw", s.fw);
  putStr(d, "ip", s.ip);
  d["passthrough"] = s.passthrough;
  std::string out;
  serializeJson(d, out);
  return out;
}

namespace {

struct Def {
  const char* component;
  const char* key;
  const char* name;
  bool bridge;
  const char* tmpl;
  const char* press;
  const char* devClass;
  const char* unit;
  const char* icon;
  bool diag;
};

const Def kDefs[] = {
    // Zhalt Mosquito (unit)
    {"button", "mist_now", "Mist now", false, nullptr, R"({"action":"mist"})", nullptr, nullptr, "mdi:weather-fog", false},
    {"button", "pulse_now", "Start pulse session", false, nullptr, R"({"action":"pulse"})", nullptr, nullptr, "mdi:timer-play", false},
    {"button", "stop", "Stop", false, nullptr, R"({"action":"stop"})", nullptr, nullptr, "mdi:stop-circle", false},
    {"button", "refresh", "Refresh", false, nullptr, R"({"action":"refresh"})", nullptr, nullptr, "mdi:refresh", true},
    {"button", "test_pump", "Test pump", false, nullptr, R"({"action":"test_pump"})", nullptr, nullptr, "mdi:pump", true},
    {"button", "test_led", "Test LED", false, nullptr, R"({"action":"test_led"})", nullptr, nullptr, "mdi:led-on", true},
    {"button", "test_buzzer", "Test buzzer", false, nullptr, R"({"action":"test_buzzer"})", nullptr, nullptr, "mdi:volume-high", true},
    {"sensor", "state", "State", false, "{{ value_json.state }}", nullptr, nullptr, nullptr, "mdi:state-machine", false},
    {"sensor", "session_ends_at", "Session ends at", false, "{{ value_json.session_end }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"sensor", "last_mist", "Last mist", false, "{{ value_json.last_mist }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"sensor", "last_result", "Last result", false, "{{ value_json.last_result }}", nullptr, nullptr, nullptr, "mdi:clipboard-check", false},
    {"sensor", "unit_state", "Unit state", false, "{{ value_json.unit_state }}", nullptr, nullptr, nullptr, "mdi:spray", false},
    {"sensor", "active_cycle", "Active cycle", false, "{{ value_json.active_cycle }}", nullptr, nullptr, nullptr, "mdi:repeat", false},
    {"sensor", "unit_clock", "Unit clock", false, "{{ value_json.unit_clock }}", nullptr, nullptr, nullptr, "mdi:clock-outline", true},
    {"sensor", "unit_firmware", "Unit firmware", false, "{{ value_json.unit_firmware }}", nullptr, nullptr, nullptr, "mdi:chip", true},
    {"sensor", "last_contact", "Last contact with unit", false, "{{ value_json.last_contact }}", nullptr, "timestamp", nullptr, nullptr, false},
    {"binary_sensor", "misting", "Misting", false, "{{ 'ON' if value_json.misting else 'OFF' }}", nullptr, "running", nullptr, nullptr, false},
    // Zhalt Bridge
    {"binary_sensor", "zhalt_reachable", "Zhalt reachable", true, "{{ 'ON' if value_json.zhalt_reachable else 'OFF' }}", nullptr, "connectivity", nullptr, nullptr, true},
    {"binary_sensor", "passthrough_active", "Pass-through active", true, "{{ 'ON' if value_json.passthrough else 'OFF' }}", nullptr, "running", nullptr, nullptr, true},
    {"sensor", "wifi_signal", "Wi-Fi signal", true, "{{ value_json.rssi }}", nullptr, "signal_strength", "dBm", nullptr, true},
    {"sensor", "join_time", "Join time", true, "{{ value_json.join_ms }}", nullptr, "duration", "ms", nullptr, true},
    {"sensor", "join_failures_24h", "Join failures (24 h)", true, "{{ value_json.join_failures_24h }}", nullptr, nullptr, nullptr, "mdi:wifi-alert", true},
    {"sensor", "uptime", "Uptime", true, "{{ value_json.uptime_s }}", nullptr, "duration", "s", nullptr, true},
    {"sensor", "free_heap", "Free memory", true, "{{ value_json.free_heap }}", nullptr, "data_size", "B", nullptr, true},
    {"sensor", "reset_reason", "Reboot reason", true, "{{ value_json.reset_reason }}", nullptr, nullptr, nullptr, "mdi:restart-alert", true},
    {"sensor", "firmware", "Firmware", true, "{{ value_json.fw }}", nullptr, nullptr, nullptr, "mdi:chip", true},
    {"button", "reboot", "Reboot", true, nullptr, R"({"action":"reboot"})", "restart", nullptr, nullptr, true},
    {"button", "test_join", "Test join", true, nullptr, R"({"action":"test_join"})", nullptr, nullptr, "mdi:wifi-sync", true},
};

}  // namespace

std::vector<DiscoveryMsg> discoveryMessages(const std::string& mac, const std::string& fw, const std::string& ip) {
  std::vector<DiscoveryMsg> out;
  const std::string bridgeId = "zhalt_bridge_" + mac;

  auto base = [&](JsonDocument& doc, const char* component, const char* key, const char* name, bool bridge) {
    doc["name"] = name;
    doc["unique_id"] = "zhalt_" + mac + "_" + key;
    doc["default_entity_id"] = std::string(component) + ".zhalt_mqtt_" + key;
    JsonObject dev = doc["device"].to<JsonObject>();
    if (bridge) {
      dev["identifiers"].to<JsonArray>().add(bridgeId);
      dev["name"] = "Zhalt Bridge";
      dev["manufacturer"] = "Waveshare";
      dev["model"] = "ESP32-S3-ETH";
      dev["sw_version"] = fw;
      dev["configuration_url"] = "http://" + ip + "/";
    } else {
      dev["identifiers"].to<JsonArray>().add("zhalt_unit_" + mac);
      dev["name"] = "Zhalt Mosquito";
      dev["manufacturer"] = "Zhalt";
      dev["model"] = "Evolution Connect";
      dev["via_device"] = bridgeId;
    }
  };
  auto emit = [&](const char* component, const char* key, JsonDocument& doc) {
    DiscoveryMsg m;
    m.topic = std::string("homeassistant/") + component + "/zhalt_" + mac + "/" + key + "/config";
    serializeJson(doc, m.payload);
    out.push_back(m);
  };

  for (const Def& d : kDefs) {
    JsonDocument doc;
    base(doc, d.component, d.key, d.name, d.bridge);
    doc["availability_topic"] = kTopicStatus;
    if (d.press) {
      doc["command_topic"] = kTopicCmd;
      doc["payload_press"] = d.press;
    }
    if (d.tmpl) {
      doc["state_topic"] = kTopicState;
      doc["value_template"] = d.tmpl;
    }
    if (d.devClass) doc["device_class"] = d.devClass;
    if (d.unit) doc["unit_of_measurement"] = d.unit;
    if (d.icon) doc["icon"] = d.icon;
    if (d.diag) doc["entity_category"] = "diagnostic";
    if (std::strcmp(d.key, "last_mist") == 0) {
      doc["json_attributes_topic"] = kTopicState;
      doc["json_attributes_template"] = "{{ {'kind': value_json.last_mist_kind} | tojson }}";
    }
    emit(d.component, d.key, doc);
  }

  {
    JsonDocument doc;
    base(doc, "switch", "onboard_scheduler", "Onboard scheduler", false);
    doc["availability_topic"] = kTopicStatus;
    doc["command_topic"] = kTopicCmd;
    doc["payload_on"] = R"({"action":"scheduler","on":true})";
    doc["payload_off"] = R"({"action":"scheduler","on":false})";
    doc["state_topic"] = kTopicState;
    doc["value_template"] =
        "{% if value_json.scheduler_on is none %}None{% elif value_json.scheduler_on %}ON{% else %}OFF{% endif %}";
    doc["icon"] = "mdi:calendar-clock";
    emit("switch", "onboard_scheduler", doc);
  }
  {
    JsonDocument doc;
    base(doc, "binary_sensor", "online", "Bridge online", true);
    doc["state_topic"] = kTopicStatus;
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    doc["device_class"] = "connectivity";
    emit("binary_sensor", "online", doc);
  }
  return out;
}

}  // namespace z
```

- [ ] **Step 5: Run all native tests**

Run: `pio test -e native`
Expected: every suite PASSED (smoke, protocol, conversation, config, safety, jobs, publish).

- [ ] **Step 6: Commit**

```bash
git add firmware && git commit -m "firmware: state JSON and HA MQTT discovery payloads

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Board glue: network, NVS store, event log

From here on, code runs only on the ESP32. Verification is `pio run -e esp32s3` compiling cleanly, then the bench test (Task 11).

**Files:**
- Create: `firmware/src/net.h`, `net.cpp`, `store.h`, `store.cpp`, `eventlog.h`, `eventlog.cpp`
- Modify: `firmware/src/main.cpp` (temporary bring-up loop)

**Interfaces:**
- Produces:
  - `net::begin()`, `bool net::ethUp()`, `bool net::clockValid()`, `int64_t net::now()`
  - `String net::ethIp()`, `String net::mac()` (lower-case, no colons)
  - `bool net::joinZhalt(uint32_t timeoutMs, int& rssi, uint32_t& joinMs)`, `void net::leaveZhalt()`
  - `store::begin()`, `std::string store::get(const char*)` (`""` if missing), `void store::put(const char*, const std::string&)` (skips the write if unchanged)
  - `elog::add(const char* fmt, ...)`, `std::vector<std::string> elog::recent()` (oldest first, max 50)

W5500 pins come from the Waveshare ESP32-S3-ETH wiki: CS 14, INT 10, RST 9, SCK 13, MISO 12, MOSI 11. **Confirm against the wiki pinout table before the first flash.**

- [ ] **Step 1: Write `src/eventlog.h` and `src/eventlog.cpp`**

```cpp
// eventlog.h
#pragma once
#include <string>
#include <vector>

namespace elog {
void add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recent();
}  // namespace elog
```

```cpp
// eventlog.cpp
#include "eventlog.h"

#include <Arduino.h>

#include <cstdarg>
#include <deque>
#include <mutex>

#include "net.h"
#include "ztime.h"

static std::deque<std::string> s_lines;
static std::mutex s_mu;
static constexpr size_t kMaxLines = 50;

void elog::add(const char* fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char line[200];
  if (net::clockValid()) {
    z::LocalDateTime t = z::toLocal(net::now());
    snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d %s", t.month, t.day, t.hour, t.minute, t.second, msg);
  } else {
    snprintf(line, sizeof line, "+%lus %s", (unsigned long)(millis() / 1000), msg);
  }
  Serial.println(line);
  std::lock_guard<std::mutex> l(s_mu);
  s_lines.push_back(line);
  while (s_lines.size() > kMaxLines) s_lines.pop_front();
}

std::vector<std::string> elog::recent() {
  std::lock_guard<std::mutex> l(s_mu);
  return {s_lines.begin(), s_lines.end()};
}
```

- [ ] **Step 2: Write `src/store.h` and `src/store.cpp`**

```cpp
// store.h
#pragma once
#include <string>

namespace store {
void begin();
std::string get(const char* key);
void put(const char* key, const std::string& value);
}  // namespace store
```

```cpp
// store.cpp
#include "store.h"

#include <Preferences.h>

#include <mutex>

static Preferences s_prefs;
static std::mutex s_mu;

void store::begin() { s_prefs.begin("zhalt", false); }

std::string store::get(const char* key) {
  std::lock_guard<std::mutex> l(s_mu);
  if (!s_prefs.isKey(key)) return "";
  return s_prefs.getString(key, "").c_str();
}

void store::put(const char* key, const std::string& value) {
  std::lock_guard<std::mutex> l(s_mu);
  if (s_prefs.isKey(key) && s_prefs.getString(key, "") == value.c_str()) return;  // spare the flash
  s_prefs.putString(key, value.c_str());
}
```

- [ ] **Step 3: Write `src/net.h` and `src/net.cpp`**

```cpp
// net.h
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
```

```cpp
// net.cpp
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
```

- [ ] **Step 4: Temporary bring-up `src/main.cpp`**

```cpp
#include <Arduino.h>

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
```

- [ ] **Step 5: Compile**

Run: `pio run -e esp32s3`
Expected: `SUCCESS`. If `ETH.begin` or `ETH.setDefault` signatures differ in the resolved core version, check them against `~/.platformio/packages/framework-arduinoespressif32/libraries/Ethernet/src/ETH.h` and `.../Network/src/NetworkInterface.h`, and adapt.

- [ ] **Step 6: Commit**

```bash
git add firmware && git commit -m "firmware: Ethernet, on-demand Zhalt Wi-Fi, NVS store, event log

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: App state, session runner, executor

**Files:**
- Create: `firmware/src/app.h`, `app.cpp`, `session.h`, `session.cpp`, `executor.h`, `executor.cpp`, `passthrough.h` (header only for now; implementation in Task 10)

**Interfaces:**
- Consumes: all of `zcore`; `net`, `store`, `elog`.
- Produces:
  - `struct App` and global `app`
  - `appInit()`, `appSubmit(const z::Command&)`, `appApplyConfig(const std::string&)`, `appTickSafety()`
  - `appRecordJoin(bool, int, uint32_t)`, `appSetResult(const std::string&)`, `std::string appStateJson()`
  - `session::run(z::Conversation&, uint32_t timeoutMs, const std::function<bool()>& stopNow)` → `session::Outcome`
  - `session::kJoinTimeoutMs`
  - `executorStart()`
  - `passthrough::begin()`, `passthrough::poll()`, `passthrough::run()`, `bool passthrough::active()`

**Locking rule:** `app.mu` guards `queue`, `safety`, `snap`, `cachedOriginal`, `joinFailures`, `savedSafety`, `mistEndEpoch`. Never call an `app*()` function while holding `app.mu` (std::mutex is not recursive). `elog::add` and `store::*` are safe under the lock.

- [ ] **Step 1: Write `src/passthrough.h`** (implementation comes in Task 10; a stub `.cpp` lets this task link)

```cpp
// passthrough.h
#pragma once

namespace passthrough {
void begin();   // listen on TCP :81 (Ethernet side)
void poll();    // from loop(): accept a client and queue a Passthrough job
void run();     // from the executor: join, splice to the unit, leave
bool active();
}  // namespace passthrough
```

`src/passthrough.cpp` (stub, replaced in Task 10):
```cpp
#include "passthrough.h"

void passthrough::begin() {}
void passthrough::poll() {}
void passthrough::run() {}
bool passthrough::active() { return false; }
```

- [ ] **Step 2: Write `src/app.h`**

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "zconfig.h"
#include "zjobs.h"
#include "zprotocol.h"
#include "zpublish.h"
#include "zsafety.h"

struct App {
  std::mutex mu;  // guards the non-atomic fields below
  z::JobQueue queue;
  z::Safety safety;
  z::BridgeSnapshot snap;
  z::Settings cachedOriginal;  // unit settings seen with cycles on; restores "scheduler on"
  std::deque<int64_t> joinFailures;
  std::string savedSafety;
  int64_t mistEndEpoch = 0;
  std::atomic<bool> preempt{false};  // a stop is waiting: running job should yield
  std::atomic<bool> dirty{true};     // state changed: publish
  std::atomic<bool> rebootRequested{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> mistActive{false};
};

extern App app;

void appInit();
void appSubmit(const z::Command& c);
void appApplyConfig(const std::string& json);
void appTickSafety();
void appRecordJoin(bool ok, int rssi, uint32_t joinMs);
void appSetResult(const std::string& result);
std::string appStateJson();
```

- [ ] **Step 3: Write `src/app.cpp`**

```cpp
#include "app.h"

#include <Arduino.h>
#include <esp_system.h>

#include "eventlog.h"
#include "net.h"
#include "passthrough.h"
#include "store.h"
#include "version.h"

App app;

static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "external";
    default: return "other";
  }
}

void appInit() {
  std::lock_guard<std::mutex> l(app.mu);
  z::Config cfg = z::defaultConfig();
  std::string err, saved = store::get("config");
  if (!saved.empty() && !z::parseConfig(saved, cfg, err)) elog::add("stored config ignored: %s", err.c_str());
  app.safety.setConfig(cfg);
  z::SafetyState st;
  if (z::Safety::deserialize(store::get("safety"), st)) {
    app.safety.restore(st);
    elog::add("safety restored: %s", store::get("safety").c_str());
  }
  app.savedSafety = app.safety.serialize();
  z::Settings s;
  if (z::parseGImp(store::get("imp"), s) && s.valid) app.cachedOriginal = s;
  app.snap.lastMist = atoll(store::get("lastmist").c_str());
  app.snap.lastMistKind = store::get("lastkind");
  app.snap.lastResult = store::get("lastres");
  app.snap.fw = ZB_FW_VERSION;
  app.snap.resetReason = resetReasonStr();
}

void appSubmit(const z::Command& c) {
  using z::CmdType;
  using z::JobType;
  std::lock_guard<std::mutex> l(app.mu);
  z::Job j;
  switch (c.type) {
    case CmdType::Stop:
      app.safety.requestStop(net::now());
      app.preempt = true;
      app.dirty = true;
      return;
    case CmdType::Reboot: app.rebootRequested = true; return;
    case CmdType::Mist: j.type = JobType::Mist; j.seconds = c.seconds; break;
    case CmdType::Pulse: j.type = JobType::Pulse; j.minutes = c.minutes; break;
    case CmdType::Refresh: j.type = JobType::Refresh; break;
    case CmdType::Scheduler: j.type = JobType::Scheduler; j.on = c.on; break;
    case CmdType::TestPump: j.type = JobType::TestPump; break;
    case CmdType::TestLed: j.type = JobType::TestLed; break;
    case CmdType::TestBuzzer: j.type = JobType::TestBuzzer; break;
    case CmdType::TestJoin: j.type = JobType::TestJoin; break;
    case CmdType::Invalid: return;
  }
  if (!app.queue.push(j)) {
    app.snap.lastResult = "queue_full";
    elog::add("queue full, command dropped");
  }
  app.dirty = true;
}

void appApplyConfig(const std::string& json) {
  std::lock_guard<std::mutex> l(app.mu);
  z::Config c = app.safety.config();
  std::string err;
  if (!z::parseConfig(json, c, err)) {
    elog::add("config rejected: %s", err.c_str());
    return;
  }
  app.safety.setConfig(c);
  store::put("config", z::configToJson(c));
  elog::add("config applied");
}

void appTickSafety() {
  std::lock_guard<std::mutex> l(app.mu);
  z::TickResult r = app.safety.tick(net::now(), net::clockValid());
  if (r.action == z::TickAction::Stop) {
    z::Job j;
    j.type = z::JobType::Stop;
    app.queue.push(j);
    app.preempt = true;
    elog::add("safety: stop queued");
  } else if (r.action == z::TickAction::ResumePulse) {
    z::Job j;
    j.type = z::JobType::Pulse;
    j.minutes = r.minutes;
    j.resume = true;
    app.queue.push(j);
    elog::add("safety: resume pulse for %d min", r.minutes);
  }
  std::string s = app.safety.serialize();
  if (s != app.savedSafety) {
    store::put("safety", s);
    app.savedSafety = s;
    app.dirty = true;
  }
}

void appRecordJoin(bool ok, int rssi, uint32_t joinMs) {
  std::lock_guard<std::mutex> l(app.mu);
  int64_t now = net::now();
  app.snap.zhaltReachable = ok;
  if (ok) {
    app.snap.rssiKnown = true;
    app.snap.rssi = rssi;
    app.snap.joinMs = joinMs;
  } else {
    app.joinFailures.push_back(now);
  }
  while (!app.joinFailures.empty() && now - app.joinFailures.front() > 86400) app.joinFailures.pop_front();
  app.snap.joinFailures24h = int(app.joinFailures.size());
  app.dirty = true;
}

void appSetResult(const std::string& result) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastResult = result;
  store::put("lastres", result);
  app.dirty = true;
}

std::string appStateJson() {
  std::lock_guard<std::mutex> l(app.mu);
  z::BridgeSnapshot s = app.snap;
  const z::SafetyState& st = app.safety.state();
  bool mist = app.mistActive;
  s.state = z::deriveState(st, mist);
  s.misting = mist || st.pulseActive;
  s.sessionEnd = mist ? app.mistEndEpoch : (st.pulseActive ? st.pulseEndEpoch : 0);
  s.uptimeS = millis() / 1000;
  s.freeHeap = ESP.getFreeHeap();
  s.ip = net::ethIp().c_str();
  s.passthrough = passthrough::active();
  return z::stateJson(s);
}
```

- [ ] **Step 4: Write `src/session.h` and `src/session.cpp`**

```cpp
// session.h
#pragma once
#include <cstdint>
#include <functional>

#include "zconversation.h"

namespace session {
constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr uint32_t kWsConnectTimeoutMs = 8000;

struct Outcome {
  bool joined = false;
  bool wsConnected = false;
  bool wsDropped = false;
  int rssi = 0;
  uint32_t joinMs = 0;
};

// Join the Zhalt AP, run the conversation until it finishes or times out, then leave.
// stopNow is polled every loop; true asks the conversation to stop the unit.
Outcome run(z::Conversation& conv, uint32_t timeoutMs, const std::function<bool()>& stopNow);
}  // namespace session
```

```cpp
// session.cpp
#include "session.h"

#include <Arduino.h>
#include <WebSocketsClient.h>

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
    ws.setReconnectInterval(60000);
    ws.begin("172.217.28.1", 81, "/");
    uint32_t t0 = millis();
    while (conv.status() == z::Conversation::Status::Running && millis() - t0 < timeoutMs && !o.wsDropped) {
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
  net::leaveZhalt();
  return o;
}
```

- [ ] **Step 5: Write `src/executor.h` and `src/executor.cpp`**

```cpp
// executor.h
#pragma once
void executorStart();
```

```cpp
// executor.cpp
#include "executor.h"

#include <Arduino.h>

#include <algorithm>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "passthrough.h"
#include "session.h"
#include "store.h"
#include "zconversation.h"

using z::JobType;
using St = z::Conversation::Status;

static const char* jobName(JobType t) {
  switch (t) {
    case JobType::Mist: return "mist";
    case JobType::Pulse: return "pulse";
    case JobType::Stop: return "stop";
    case JobType::Refresh: return "refresh";
    case JobType::Scheduler: return "scheduler";
    case JobType::TestPump: return "test_pump";
    case JobType::TestLed: return "test_led";
    case JobType::TestBuzzer: return "test_buzzer";
    case JobType::TestJoin: return "test_join";
    case JobType::Passthrough: return "passthrough";
  }
  return "?";
}

static const char* gateResult(z::Gate g) {
  switch (g) {
    case z::Gate::ClockNotSet: return "clock_not_set";
    case z::Gate::Blackout: return "blackout";
    case z::Gate::PulseActive: return "pulse_active";
    default: return "ok";
  }
}

static z::Goal goalFor(JobType t) {
  switch (t) {
    case JobType::Mist: return z::Goal::Mist;
    case JobType::Pulse: return z::Goal::Pulse;
    case JobType::Stop: return z::Goal::Stop;
    case JobType::Scheduler: return z::Goal::Scheduler;
    case JobType::TestPump: return z::Goal::TestPump;
    case JobType::TestLed: return z::Goal::TestLed;
    case JobType::TestBuzzer: return z::Goal::TestBuzzer;
    default: return z::Goal::Refresh;
  }
}

static void absorbUnitData(const z::Conversation& c) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastContact = net::now();
  if (c.haveDat()) {
    const z::Dat& d = c.lastDat();
    app.snap.unitValid = true;
    app.snap.unitOp = d.op_mode;
    app.snap.unitActiveCycle = d.active_cycle;
    char buf[24];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", d.dev_year, d.dev_month, d.dev_day, d.dev_hour,
             d.dev_minute, d.dev_second);
    app.snap.unitClock = buf;
  }
  const z::Settings& s = c.settings();
  if (s.valid) {
    app.snap.unitFirmware = s.firmware_minor;
    app.snap.schedulerOn = z::anyCycleActive(s) ? 1 : 0;
    if (!app.cachedOriginal.valid && z::anyCycleActive(s)) {
      app.cachedOriginal = s;
      store::put("imp", s.raw);
      elog::add("cached unit settings for scheduler restore");
    }
  }
  app.dirty = true;
}

static void recordMist(const char* kind) {
  std::lock_guard<std::mutex> l(app.mu);
  app.snap.lastMist = net::now();
  app.snap.lastMistKind = kind;
  store::put("lastmist", std::to_string(app.snap.lastMist));
  store::put("lastkind", kind);
}

static void runConversation(const z::Job& job) {
  z::Conversation conv(goalFor(job.type));
  uint32_t timeoutMs = 30000;
  int secs = 0, minutes = 0;
  z::Settings cached;
  {
    std::lock_guard<std::mutex> l(app.mu);
    if (job.type == JobType::Mist || job.type == JobType::Pulse) {
      z::Gate g = app.safety.canStart(net::now(), net::clockValid());
      if (g != z::Gate::Ok) {
        app.snap.lastResult = gateResult(g);
        app.dirty = true;
        elog::add("%s refused: %s", jobName(job.type), gateResult(g));
        return;
      }
    }
    if (job.type == JobType::Mist) {
      secs = app.safety.clampMistSeconds(job.seconds);
      conv.setMistDurationMs(uint32_t(secs) * 1000);
      timeoutMs = uint32_t(secs) * 1000 + 60000;
    }
    if (job.type == JobType::Pulse) {
      minutes = job.resume ? std::max(1, job.minutes) : app.safety.clampPulseMinutes(job.minutes);
      timeoutMs = 60000;
    }
    if (job.type == JobType::Stop) timeoutMs = 45000;
    if (job.type == JobType::Scheduler) {
      if (job.on && !app.cachedOriginal.valid) {
        app.snap.lastResult = "no_cached_settings";
        app.dirty = true;
        return;
      }
      cached = app.cachedOriginal;
    }
  }
  if (job.type == JobType::Scheduler) conv.setSchedulerTarget(job.on, job.on ? &cached : nullptr);

  bool markedStart = false;
  auto stopNow = [&]() -> bool {
    if (job.type == JobType::Mist && conv.started() && !markedStart) {
      markedStart = true;
      std::lock_guard<std::mutex> l(app.mu);
      app.mistActive = true;
      app.mistEndEpoch = net::now() + secs;
      app.dirty = true;
    }
    if (job.type == JobType::Stop) return false;
    if (app.preempt) return true;
    if (job.type == JobType::Mist) {
      std::lock_guard<std::mutex> l(app.mu);
      return app.safety.inBlackout(net::now());
    }
    return false;
  };

  elog::add("job %s start", jobName(job.type));
  session::Outcome o = session::run(conv, timeoutMs, stopNow);
  {
    std::lock_guard<std::mutex> l(app.mu);
    app.mistActive = false;
    app.mistEndEpoch = 0;
  }
  appRecordJoin(o.joined, o.rssi, o.joinMs);
  int64_t now = net::now();
  bool done = conv.status() == St::Done;
  std::string err = !o.joined                       ? "join_failed"
                    : !o.wsConnected                ? "ws_connect_failed"
                    : conv.status() == St::Running  ? (o.wsDropped ? "ws_dropped" : "timeout")
                                                    : conv.error();
  if (o.wsConnected) absorbUnitData(conv);

  switch (job.type) {
    case JobType::Mist:
      if (conv.started()) recordMist("mist");
      if (done) {
        appSetResult(conv.started() ? "ok" : "cancelled");
      } else {
        // The mist flag may have landed; manual mist has no onboard timeout. Make sure it stops.
        if (o.wsConnected) {
          std::lock_guard<std::mutex> l(app.mu);
          app.safety.requestStop(now);
        }
        appSetResult(err);
      }
      break;
    case JobType::Pulse:
      if (done && conv.started()) {
        {
          std::lock_guard<std::mutex> l(app.mu);
          app.safety.onPulseStarted(now, minutes);
        }
        recordMist("pulse");
        appSetResult("ok");
      } else {
        appSetResult(done ? "cancelled" : err);
      }
      break;
    case JobType::Stop: {
      {
        std::lock_guard<std::mutex> l(app.mu);
        if (done)
          app.safety.onStopConfirmed();
        else
          app.safety.onStopFailed(now);
      }
      appSetResult(done ? "ok" : "stop_attempt_failed:" + err);
      break;
    }
    default:
      appSetResult(done ? "ok" : err);
  }
  elog::add("job %s -> %s", jobName(job.type), done ? "ok" : err.c_str());
}

static void runJob(const z::Job& job) {
  switch (job.type) {
    case JobType::TestJoin: {
      int rssi = 0;
      uint32_t ms = 0;
      bool ok = net::joinZhalt(session::kJoinTimeoutMs, rssi, ms);
      net::leaveZhalt();
      appRecordJoin(ok, rssi, ms);
      appSetResult(ok ? "ok" : "join_failed");
      return;
    }
    case JobType::Passthrough:
      passthrough::run();
      return;
    default:
      runConversation(job);
  }
}

static void executorTask(void*) {
  for (;;) {
    z::Job job;
    bool have;
    {
      std::lock_guard<std::mutex> l(app.mu);
      have = app.queue.pop(job);
    }
    if (!have) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (job.type == JobType::Stop) app.preempt = false;
    app.busy = true;
    app.dirty = true;
    runJob(job);
    app.busy = false;
    app.dirty = true;
  }
}

void executorStart() { xTaskCreatePinnedToCore(executorTask, "executor", 12288, nullptr, 1, nullptr, 1); }
```

- [ ] **Step 6: Wire into `src/main.cpp`** (MQTT and web come in Tasks 9–10)

```cpp
#include <Arduino.h>

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
```

- [ ] **Step 7: Compile and re-run the native tests**

Run: `pio run -e esp32s3 && pio test -e native`
Expected: build `SUCCESS`; all native suites PASSED.

- [ ] **Step 8: Commit**

```bash
git add firmware && git commit -m "firmware: app state, session runner and job executor

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: MQTT

**Files:**
- Create: `firmware/src/mqtt.h`, `mqtt.cpp`
- Modify: `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `appSubmit`, `appApplyConfig`, `appStateJson`, `z::discoveryMessages`, `z::parseCommand`, topic constants.
- Produces: `mqtt::begin()`, `mqtt::loop()`, `mqtt::publishOffline()`.

- [ ] **Step 1: Write `src/mqtt.h` and `src/mqtt.cpp`**

```cpp
// mqtt.h
#pragma once

namespace mqtt {
void begin();
void loop();
void publishOffline();
}  // namespace mqtt
```

```cpp
// mqtt.cpp
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
  s_mqtt.setCallback(onMessage);
}

void mqtt::publishOffline() {
  if (s_mqtt.connected()) s_mqtt.publish(z::kTopicStatus, "offline", true);
}

void mqtt::loop() {
  if (!net::ethUp()) return;
  if (!s_mqtt.connected()) {
    if (millis() - s_lastAttempt < 5000) return;
    s_lastAttempt = millis();
    String id = "zhalt-bridge-" + net::mac();
    if (!s_mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, z::kTopicStatus, 1, true, "offline")) {
      elog::add("mqtt connect failed rc=%d", s_mqtt.state());
      return;
    }
    s_mqtt.publish(z::kTopicStatus, "online", true);
    // A retained command would replay on every reconnect: clear it before subscribing.
    s_mqtt.publish(z::kTopicCmd, "", true);
    for (const auto& m : z::discoveryMessages(net::mac().c_str(), ZB_FW_VERSION, net::ethIp().c_str()))
      s_mqtt.publish(m.topic.c_str(), m.payload.c_str(), true);
    s_mqtt.subscribe(z::kTopicCmd, 1);
    s_mqtt.subscribe(z::kTopicConfig, 1);
    elog::add("mqtt connected");
    app.dirty = true;
  }
  s_mqtt.loop();
  if (app.dirty.exchange(false) || millis() - s_lastPublish > 60000) {
    std::string js = appStateJson();
    s_mqtt.publish(z::kTopicState, js.c_str(), true);
    s_lastPublish = millis();
  }
}
```

- [ ] **Step 2: Add MQTT to `src/main.cpp`**

Add `#include "mqtt.h"`. In `setup()`, call `mqtt::begin();` after `appInit();`. In `loop()`, call `mqtt::loop();` as the first line. Before `ESP.restart()`, call `mqtt::publishOffline();`.

- [ ] **Step 3: Compile**

Run: `pio run -e esp32s3`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add firmware && git commit -m "firmware: MQTT discovery, commands, config and state

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Web page, firmware upload, pass-through

**Files:**
- Create: `firmware/src/web.h`, `web.cpp`
- Modify: `firmware/src/passthrough.cpp` (replace the stub), `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `appStateJson`, `appSubmit`, `appRecordJoin`, `app`, `elog::recent`, `net`.
- Produces:
  - `web::begin()`
  - HTTP endpoints:
    - `GET /`
    - `GET /status.json`
    - `POST /reboot`, `POST /test-join`, `POST /update` (basic auth)
  - The real `passthrough::*`.

- [ ] **Step 1: Write `src/web.h` and `src/web.cpp`**

```cpp
// web.h
#pragma once

namespace web {
void begin();
}
```

```cpp
// web.cpp
#include "web.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include "app.h"
#include "eventlog.h"
#include "secrets.h"
#include "zconfig.h"

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

static bool authed(AsyncWebServerRequest* r) {
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
        bool ok = !Update.hasError();
        r->send(ok ? 200 : 500, "text/plain", ok ? "ok, rebooting when idle" : "update failed");
        if (ok) app.rebootRequested = true;
      },
      [](AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!r->authenticate(WEB_USER, WEB_PASS)) return;
        if (index == 0) {
          elog::add("firmware upload %s", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) elog::add("update begin failed");
        }
        if (Update.isRunning() && Update.write(data, len) != len) elog::add("update write failed");
        if (final) {
          if (Update.end(true))
            elog::add("update ok, %u bytes", unsigned(index + len));
          else
            elog::add("update end failed");
        }
      });
  s_server.begin();
}
```

- [ ] **Step 2: Replace `src/passthrough.cpp`**

```cpp
#include "passthrough.h"

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>

#include "app.h"
#include "eventlog.h"
#include "net.h"
#include "session.h"

static WiFiServer s_server(81);
static WiFiClient s_pending;
static std::atomic<bool> s_hasPending{false}, s_active{false};
static constexpr uint32_t kIdleLimitMs = 10 * 60 * 1000;

void passthrough::begin() { s_server.begin(); }

bool passthrough::active() { return s_active; }

void passthrough::poll() {
  WiFiClient c = s_server.accept();
  if (!c) return;
  if (s_hasPending || s_active) {
    c.stop();  // one client at a time, like the unit itself
    return;
  }
  s_pending = c;
  s_hasPending = true;
  z::Job j;
  j.type = z::JobType::Passthrough;
  bool queued;
  {
    std::lock_guard<std::mutex> l(app.mu);
    queued = app.queue.push(j);
  }
  if (!queued) {
    s_pending.stop();
    s_hasPending = false;
  }
}

void passthrough::run() {
  WiFiClient lan = s_pending;
  s_hasPending = false;
  s_active = true;
  app.dirty = true;
  elog::add("passthrough start");
  int rssi = 0;
  uint32_t ms = 0;
  bool joined = net::joinZhalt(session::kJoinTimeoutMs, rssi, ms);
  appRecordJoin(joined, rssi, ms);
  WiFiClient unit;
  if (joined && unit.connect(IPAddress(172, 217, 28, 1), 81, 5000)) {
    uint8_t buf[512];
    uint32_t lastIo = millis();
    while (lan.connected() && unit.connected() && !app.preempt && millis() - lastIo < kIdleLimitMs) {
      int n;
      while ((n = lan.available()) > 0) {
        n = lan.read(buf, n > 512 ? 512 : n);
        unit.write(buf, n);
        lastIo = millis();
      }
      while ((n = unit.available()) > 0) {
        n = unit.read(buf, n > 512 ? 512 : n);
        lan.write(buf, n);
        lastIo = millis();
      }
      delay(2);
    }
  } else {
    elog::add("passthrough: unit not reachable");
  }
  unit.stop();
  lan.stop();
  net::leaveZhalt();
  s_active = false;
  app.dirty = true;
  elog::add("passthrough end");
}
```

- [ ] **Step 3: Add web to `src/main.cpp`**

Add `#include "web.h"` and call `web::begin();` in `setup()` right after `net::begin();`.

Final `src/main.cpp`:
```cpp
#include <Arduino.h>

#include "app.h"
#include "eventlog.h"
#include "executor.h"
#include "mqtt.h"
#include "net.h"
#include "passthrough.h"
#include "store.h"
#include "version.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  store::begin();
  net::begin();
  web::begin();
  appInit();
  mqtt::begin();
  passthrough::begin();
  executorStart();
  elog::add("boot fw %s", ZB_FW_VERSION);
}

void loop() {
  static uint32_t lastTick = 0;
  mqtt::loop();
  passthrough::poll();
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    appTickSafety();
  }
  if (app.rebootRequested && !app.busy) {
    mqtt::publishOffline();
    delay(300);
    ESP.restart();
  }
  delay(5);
}
```

- [ ] **Step 4: Compile and run the native tests**

Run: `pio run -e esp32s3 && pio test -e native`
Expected: build `SUCCESS`, flash usage under 50% of the app partition (shown in the build output); all native suites PASSED.

- [ ] **Step 5: Commit**

```bash
git add firmware && git commit -m "firmware: status page, firmware upload, TCP pass-through

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Flash and bench test (board on the desk, no Zhalt)

Needs the board. Steps marked **(Simon)** need Simon.

- [ ] **Step 1 (Simon): Antenna resistor.** Photograph the antenna-select resistor near the U.FL socket before first power-up. If it selects the PCB antenna, move it to the U.FL position, then fit the antenna.

- [ ] **Step 2 (Simon): MQTT user.** Create a HA user `zhalt-bridge` (Mosquito broker accepts HA users) or a Mosquitto login. Put the credentials and a web password in `firmware/include/secrets.h`.

- [ ] **Step 3: First flash over USB** (board on the Mac mini's USB-C, no PoE yet)

Run: `pio run -e esp32s3 -t upload`
Expected: `SUCCESS`. If the port isn't found, hold BOOT while plugging in, then retry.

Watch the serial log in the background. Record the PID and stop it later with `kill <pid>` (never `pkill`):
```bash
pio device monitor -e esp32s3 --raw > .pio/serial.log 2>&1 & echo $!
# if it refuses without a TTY: script -q /dev/null pio device monitor -e esp32s3 --raw > .pio/serial.log 2>&1 & echo $!
```
Expected in the log: `boot fw 0.1.0`.

- [ ] **Step 4 (Simon): Ethernet + reservation.** Plug the board into Server Switch port 6 with PoE on. Read the MAC from the serial log (`mac=` line from the eth-up/boot output) or from UniFi. In UniFi, delete the stale Galaxy Tab reservation for 10.0.0.30 and reserve 10.0.0.30 for the board's MAC. Power-cycle the port.
  Expected: `curl -s http://10.0.0.30/status.json` returns JSON with `"ip":"10.0.0.30"` and `fw` `0.1.0`.

- [ ] **Step 5: Clock, MQTT and discovery.**
  - `status.json` `uptime_s` increases.
  - The log shows `mqtt connected`.
  - In HA (MCP `ha_search` for `zhalt_mqtt`):
    - 30 entities across devices `Zhalt Mosquito` and `Zhalt Bridge`
    - `binary_sensor.zhalt_mqtt_online` = on
    - `sensor.zhalt_mqtt_state` = idle
  - If entity IDs aren't `*.zhalt_mqtt_*` (HA ignored `default_entity_id`), add `"object_id": "zhalt_mqtt_<key>"` next to `default_entity_id` in `zpublish.cpp`, re-test and reflash.

- [ ] **Step 6: Retained-command guard (Review Focus 1).**

```bash
brew install mosquitto
mosquitto_pub -h 10.0.0.9 -u <user> -P <pass> -t zhalt/cmd -r -m '{"action":"refresh"}'
```
Expected: the board runs one refresh (it ends `join_failed`, since there's no Zhalt in range). Then reboot via the web page.
Expected after reboot: no new `job refresh` in the log, and `mosquitto_sub -h 10.0.0.9 -u <user> -P <pass> -t zhalt/cmd -v -C 1 -W 3` receives nothing retained.

- [ ] **Step 7: Clean failure with no Zhalt in range.** Send `{"action":"test_join"}`.
  Expected:
  - `last_result` = `join_failed`
  - `zhalt_reachable` false
  - `join_failures_24h` 1
  - The board's MQTT stays connected, which proves Ethernet stayed the default route while Wi-Fi was trying.

- [ ] **Step 8: Gates.**
  - Send `{"action":"mist","seconds":5}` while the board time is inside a blackout window. Simplest way: publish a config whose first window covers now, e.g. `{"blackouts":[{"start":"00:00","end":"23:59"}]}`. Expected: `last_result` = `blackout`, no join attempt in the log.
  - Then restore the defaults by publishing the default config JSON from the Global Constraints.

- [ ] **Step 9: Safety persistence across reboot (Review Focus 2).**
  - Publish config with resume 0.
  - Send `{"action":"stop"}`, which queues a stop that fails (no unit).
  - Within 20 s, reboot via the web page.
  - Expected after boot:
    - the log shows `safety restored` with `"sp":true`
    - a stop job runs again
    - after 4 failed attempts, `state` = `stop_failed`

- [ ] **Step 10: Firmware upload over the network.** Bump `ZB_FW_VERSION` to `0.1.1`, then:

```bash
pio run -e esp32s3 && curl -u admin:<pass> -F "firmware=@.pio/build/esp32s3/firmware.bin" http://10.0.0.30/update
```
Expected: `ok, rebooting when idle`; `status.json` `fw` = `0.1.1`.

- [ ] **Step 11: Stop the serial monitor** you started (`kill <pid>` from step 3). Commit any fixes found on the bench.

---

### Task 12: Dry run at the unit (no misting)

Needs the board mounted, the antenna by the window, and the Zhalt powered. Only `test_join` and `refresh` are sent; neither makes the unit spray.

- [ ] **Step 1: Test join.** Send `{"action":"test_join"}`.
  Expected:
  - `zhalt_reachable` true
  - `rssi` better than −75 dBm (otherwise reposition the antenna)
  - `join_ms` < 10000

- [ ] **Step 2: Refresh.** Send `{"action":"refresh"}`.
  Expected:
  - `last_result` `ok`
  - `unit_state` Standby
  - `unit_firmware` populated
  - `unit_clock` within a minute of now (the handshake set it)
  - `scheduler_on` true or false
  - If `scheduler_on` is true, the log shows `cached unit settings for scheduler restore`.

- [ ] **Step 3: The board leaves the AP.** Within 30 s of the refresh, the Zhalt AP accepts a phone. Simon opens the Zhalt app on his phone and it connects.

- [ ] **Step 4: Record results** (RSSI, join time, unit firmware, scheduler state) in the Zhalt memory file. Then stop: the supervised mist test needs Simon's explicit OK and is part of plan 2.

---

### Task 13: Hand-off to plan 2 (HA cut-over)

- [ ] **Step 1:** With real entity IDs confirmed on the bench, write `docs/superpowers/plans/<date>-esp32-bridge-ha-cutover.md` covering spec §5 and §6.4–6.5:
  - New helper `input_number.zhalt_blackout_resume_min_minutes`.
  - `zhalt_publish_bridge_config` automation.
  - Rewritten `script.zhalt_safe_mist`, `script.zhalt_safe_pulse_session` and `script.zhalt_safe_toggle_scheduler`. Read the live configs first.
  - New alert and stop-failed automations.
  - Removals: self-heal reload, Opal PoE schedule, 4-min lead, Opal ping.
  - `sensor.zhalt_next_spray` fix.
  - Dashboard repointing on `zhalt-mobile`, `office-ipad` view 26 and `kitchen-ipad-new` views 0 and 25.
  - The supervised test session.
  - Disabling the HACS integration.
  - Removing the UDM route.
  - Spray automations stay off until Simon says otherwise.
