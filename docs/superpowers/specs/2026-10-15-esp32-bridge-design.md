# Zhalt ESP32 bridge — design

Date: 2026-10-15. Status: draft for Simon's review.

## 1. Goal

Replace the GL.iNet Opal bridge with a Waveshare ESP32-S3-ETH + PoE Module (B) that talks to the Zhalt
itself and exposes it to Home Assistant over MQTT. Sessions start in seconds instead of after a ~210 s
cold boot, and the board can end a pulse session on time (the dog-window problem of 2026-10-11).

Success means:
- HA mists and pulse sessions start within ~15 s of the command, with no PoE cycling.
- Every board-started session ends on the board's own timer, confirmed by Shelly power, even if HA is down.
- The bridge never stays on the Zhalt AP longer than one command needs (manual mist excepted, see §4).
- The existing dashboards and helpers keep working, repointed at the new entities.

## 2. Decisions already made

| Decision | Choice |
|---|---|
| Firmware scope | Protocol on the board + MQTT (old "phase B") and TCP pass-through (old "phase A"), built together |
| Primary control path | MQTT. Pass-through is a fallback for the HACS integration / debugging only |
| Zhalt Wi-Fi link | On demand only. Radio fully off when idle. Never held for a whole pulse session |
| Scheduler | HA stays the scheduler. Board enforces limits only |
| Bridge IP | 10.0.0.30 via UDM DHCP reservation (replaces the stale Galaxy Tab reservation) |
| First flash | USB-C from the Mac mini (code lives on the external drive); OTA afterwards |
| HACS integration | Disabled after the supervised test passes, left installed |
| Testing on the unit | One supervised session with Simon's OK and the garden clear |

## 3. Hardware and network

- Waveshare ESP32-S3-ETH (W5500 Ethernet over SPI) + PoE Module (B), on Server Switch port 6 with PoE
  permanently on. U.FL 2.4 GHz antenna, 15 cm, by the window. **Before first power-up: photograph the
  antenna-select resistor** and move it to the U.FL position if it is on the PCB antenna.
- Ethernet is the default route (LAN, MQTT, NTP, web). Wi-Fi STA joins `ZHALT-EVO_61B3z` only on demand and
  is never the default route; the Zhalt is reached at `172.217.28.1:81`.
- After cut-over: remove the UDM static route 172.217.28.0/24 → 10.0.0.30.
- The phone app keeps working whenever the board is idle, because the board is then off the AP.

## 4. Firmware

PlatformIO project in `firmware/` in this repo. Arduino core 3.x (ESP-IDF 5, pioarduino platform) for
native W5500 support in `ETH.h`. Libraries: arduinoWebSockets (client to the Zhalt), espMqttClient,
ESPAsyncWebServer + AsyncTCP (status page, JSON, OTA upload), ArduinoJson. Small modules, each testable on
its own:

| Module | Job |
|---|---|
| `net` | Ethernet up at boot; NTP (Asia/Dubai, UTC+4); `join()` / `leave()` the Zhalt AP with timeout and RSSI capture |
| `zhalt_protocol` | Pure functions ported from `protocol.py`: `P_imp` handshake (carries date/time from the board clock), `P_dat` builder with action flags, `G_imp` / `G_dat` parsers, state mapping. No I/O |
| `zhalt_session` | One WebSocket session: connect, handshake, send action, poll `P_dat` every 1.5 s, verify the expected `G_dat` state, close |
| `jobs` | Single-worker queue. One job owns the Wi-Fi link at a time. Stop jumps the queue and pre-empts a running pass-through session |
| `safety` | Session timers and limits (below); persists to NVS |
| `mqtt` | Discovery, command topic, state publishing, LWT |
| `passthrough` | TCP :81 on Ethernet; a client connection becomes a job: join, splice bytes to 172.217.28.1:81, leave on close |
| `web` | Status page, `/status.json`, ring log of the last 50 events, OTA upload, reboot / test-join buttons |

### Jobs

| Job | Link behaviour | Done when |
|---|---|---|
| `mist(seconds)` | Join, start, stay joined, send Stop at `seconds` (backstop for the unit's own timer), verify idle, leave | Unit idle |
| `pulse(minutes)` | Join, start pulse, verify it is running, leave. Board arms a session-end timer | Unit confirmed running |
| `stop` | Join, send Stop, verify stopped (`G_dat[38] == 6` or standby), leave | Verified |
| `refresh` | Join, read `G_imp` + one `G_dat`, leave | Snapshot published |
| `scheduler(on/off)` | Join, write cycle settings (`disable_all_cycles` / `restore_cycles` logic from the integration), leave | Read-back matches |
| `test_pump` / `test_led` / `test_buzzer` | Join, fire, leave | Ack in `G_dat` |
| `test_join` | Join, record RSSI and join time, leave. No protocol traffic | Joined |

After a job the board leaves the AP immediately (radio off), with a 30 s linger only if another job is
already queued.

### Safety rules (enforced on the board, independent of HA)

1. **Mist cap:** 120 s, compiled in. A longer request is clamped and logged.
2. **Pulse-session cap:** taken from the command (HA passes its helper), clamped to 10–90 min. At the cap the
   board queues a `stop` job.
3. **Blackout windows:** the board holds HA's three windows (morning 1, morning 2, evening). It refuses to
   start a mist or pulse session inside a window, and if a pulse session is running when a window starts
   it queues a `stop` at the window start. If at least 10 min of the session's cap remain when the window
   ends, it re-starts the pulse session with the remaining time. If not, the session is over. The 10 min
   threshold is a default, set from HA (`input_number.zhalt_blackout_resume_min_minutes`).
4. **Stop retries:** a failed stop is retried 3 times, 20 s apart. After the third failure the board
   publishes state `stop_failed`; HA then cuts mains at the Shelly (§5).
5. **Persistence:** the session end time, blackout windows and last results live in NVS, so a reboot
   mid-session still produces the stop.
6. **Clock:** until NTP has synced after boot, start commands are refused (result `clock_not_set`). Stop always
   works.
7. **Onboard scheduler:** the board never turns it on by itself. Default and safe state is off.

### MQTT

Base topic `zhalt/`. Availability `zhalt/bridge/status` (`online` / LWT `offline`, retained).

- `zhalt/cmd` (HA → board): JSON, e.g. `{"action":"mist","seconds":70}`, `{"action":"pulse","minutes":40}`,
  `{"action":"stop"}`, `{"action":"refresh"}`, `{"action":"scheduler","on":false}`, `test_*`, `test_join`,
  `reboot`.
- `zhalt/config` (HA → board, retained): blackout windows and mist duration, published by HA from the
  existing helpers. The board keeps the last copy in NVS.
- `zhalt/state` (board → HA, retained JSON): live state, session end, last mist, last result, unit snapshot
  with its timestamp, bridge diagnostics.

HA entities via MQTT discovery (two devices, entity IDs prefixed `zhalt_mqtt_` so the old integration can run
side by side until cut-over):

- **Zhalt Mosquito:**
  - buttons: Mist now, Start pulse session, Stop, Refresh, Test pump/LED/buzzer
  - switch: Onboard scheduler
  - sensors: State (`idle` / `misting` / `pulse_session` / `stopping` / `stop_failed`), Session ends at,
    Last mist, Last result, Unit state, Active cycle, Unit clock, Unit firmware, Last contact with unit
  - binary sensor: Misting
- **Zhalt Bridge:**
  - binary sensors: Online, Zhalt reachable, Pass-through active
  - sensors: Wi-Fi signal, Join time, Join failures (24 h), Uptime, Free memory, Reboot reason, Firmware
  - buttons: Reboot, Test join

Mist duration, pulse length and blackout times are **not** duplicated as board entities. They stay in the
existing HA helpers, which the dashboards already show (§5).

## 5. Home Assistant changes

Existing helpers become the source for the board's settings:

| Helper | Feeds |
|---|---|
| `input_number.zhalt_long_blast_seconds` (70) | Mist duration in `zhalt/config` and in mist commands |
| `input_number.zhalt_morning_pulse_minutes` / `_evening_` | `minutes` in pulse commands, i.e. the cap. Morning set to 40 at cut-over |
| `input_datetime.zhalt_blackout_{morning1,morning2,evening}_{start,end}` | Blackout windows in `zhalt/config` |
| **New** `input_number.zhalt_blackout_resume_min_minutes` (10) | Resume threshold in `zhalt/config` |

All timings above are defaults and can be changed later from the dashboard without new firmware. The only
fixed value is the 120 s mist cap.

Scripts keep their names, so every dashboard button that calls them keeps working:
- `script.zhalt_safe_mist`: same gates (wind, rain, skip flags, blackout, spray block). The PoE, ping and
  integration steps are replaced by publishing to `zhalt/cmd`, then waiting up to 30 s for state `misting`.
  If that doesn't arrive, it logs a skip with the board's result.
- `script.zhalt_safe_pulse_session`: same, with `pulse` and the minutes helper. No PoE-off after start.
- `script.zhalt_safe_toggle_scheduler`: publishes `scheduler`.

Automations:
- **New** `zhalt_publish_bridge_config`: on HA start and on any of the helpers above changing, publishes the
  retained `zhalt/config`.
- **New** `zhalt_stop_failed_power_cut`: bridge state `stop_failed` → the existing emergency power cut
  (Shelly off) + notification.
- **New** `zhalt_bridge_offline_alert`: bridge offline for 10 min, or Zhalt unreachable on 2 consecutive
  jobs → notification.
- **New** `zhalt_no_mist_alert`: no Shelly power draw for 2 days while the automations are enabled.
- `zhalt_safety_abort`: its stop now goes over MQTT and lands at any time.
- **Removed:**
  - `zhalt_reload_integration_before_sessions_self_heal`
  - the Opal PoE schedule
  - the 4-min cold-boot lead on start times (the pulse sun-track offset and the mist trigger times shift to
    the real mist times)
  - `binary_sensor.zhalt_opal_ping`
- `sensor.zhalt_next_spray` ignores disabled automations.
- **Spray automations stay off** after cut-over until Simon says to re-enable them.

Dashboards (same layouts; cards repointed, nothing redesigned): `zhalt-mobile`, `office-ipad` → Mosquito,
`kitchen-ipad-new` → Home and Mosquito.

| Current | Becomes |
|---|---|
| `sensor.zhalt_mosquito_state`, `binary_sensor.zhalt_mosquito_misting`, `active_cycle`, `firmware_version`, `device_clock`, `machine_type` | MQTT equivalents |
| `elapsed_seconds` / `remaining_seconds` / `planned_duration` | "Session ends at" (countdown) + Last mist |
| `binary_sensor.zhalt_opal_ping` / `zhalt_mosquito_connected` | Bridge online + Zhalt reachable |
| `button.zhalt_mosquito_stop` | MQTT Stop |
| `switch.zhalt_mosquito_onboard_scheduler` | MQTT switch (still via `script.zhalt_safe_toggle_scheduler`) |
| `zhalt.refresh` button | MQTT Refresh |
| Schedule markdown "PoE strategy" / "Opal PoE auto-schedule" text | Replaced by a bridge status line |

Additions:
- a Start pulse session button next to Mist now
- a collapsed Bridge section: signal, join failures, test join, reboot

## 6. Testing

1. **Host unit tests** (PlatformIO `native` env, Unity):
   - protocol builders and parsers against the fixtures behind `test_protocol.py` and frames from `zhalt-ws.log`
   - safety timers with a fake clock: caps, blackout stop/resume, NVS restore after reboot, clock-not-set refusal
   - the job queue: Stop pre-emption, one link owner
2. **Bench** (board on the desk, no Zhalt):
   - Ethernet + reservation IP
   - NTP time
   - MQTT discovery creates both devices
   - status page and OTA upload
   - commands to an absent AP fail cleanly with `join_failed`
3. **Dry run at the unit** (no misting, no approval needed):
   - Test join: RSSI and join time
   - Refresh: unit snapshot appears in HA
   - the board leaves the AP afterwards, and the phone app can then connect
4. **Supervised session** (one visit, Simon's OK, garden clear):
   - a 15 s mist (start, board-timed stop)
   - then a pulse session with a 10 min cap: the board starts it, leaves, rejoins at the cap and stops it
   - Shelly power must show no bursts after the stop. This is the first test of Stop against a running pulse
     session.
5. **Cut-over:**
   - disable the HACS integration
   - check the dashboards
   - remove the UDM static route
   - automations stay off until Simon re-enables them

## 7. Out of scope

- Moving the schedule onto the board or onto the unit's own cycles.
- Holding the AP link for whole sessions.
- Dry-contact remote start or other hardware options from the service technician.
- Removing the HACS integration from the repo.
