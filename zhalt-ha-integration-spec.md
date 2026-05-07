# Home Assistant Custom Integration: Zhalt Evolution Connect

**Project:** `ha-zhalt`
**Repo:** `SimonB33/ha-zhalt` (private)
**Install path in HA:** `/config/custom_components/zhalt/`
**Install method:** HACS Custom Repository

---

## 1. Goal & Background

Build a native Home Assistant integration for the **Freezanz Zhalt Evolution Connect** — an outdoor mosquito misting system installed at the user's home in Dubai. The device runs an embedded web server with a WebSocket-based control protocol on TCP port 81.

The device's onboard scheduler is too primitive for real use (fixed hh:mm times, no awareness of sunrise/sunset/seasonality/presence). Goal: take over scheduling in HA so it can:

- Trigger sprays at sunrise/sunset (offset adjustable)
- Suppress sprays based on wind speed, garden motion, door sensors, manual override toggles
- Be paused for the day via a dashboard switch
- Be turned off entirely during the worst summer months
- Eventually integrate with calendar events, presence detection, etc.

---

## 2. Network & connection details

- **Device IP:** `172.217.28.1` (the Zhalt's gateway IP on its own captive AP subnet)
- **Reachable from HA at `10.0.0.9`** via:
  - UDM SE static route (already configured): `172.217.28.0/24 → next hop 10.0.0.30`
  - GL.iNet Opal travel router at `10.0.0.30` bridges WiFi (joined to `ZHALT-EVO_61B3z`) ↔ Ethernet
- **Protocol:** WebSocket, plain text, no auth, no TLS
- **URL:** `ws://172.217.28.1:81`
- **HTTP UI** (not used by this integration but useful for sanity checks): `http://172.217.28.1/Zhalt`
- **Firmware update endpoint** (not used): `http://172.217.28.1:82/goUpdate`

The integration's config flow should accept host/port with defaults `172.217.28.1` and `81`.

The Zhalt only allows **one WiFi client at a time**. This isn't an issue at runtime because the Opal is the permanent client, but if someone manually joins `ZHALT-EVO_61B3z` from a phone, the Opal gets kicked off and the integration loses its connection until the phone disconnects.

---

## 3. Protocol specification

### 3.1 Handshake

On WebSocket connect, the server immediately sends the literal text frame:

```
CiaO
```

That's it — no version, no JSON, just `CiaO`. The client must then send a `P_imp` request to start the data stream. Without it, the server is silent indefinitely.

### 3.2 Server → Client message types

Both message types are space-separated text frames. The first token identifies the type.

#### `G_imp` — Settings dump

Sent once after the client's initial `P_imp`, then again any time settings change.

**Live captured example** (current device state, May 2026):

```
G_imp 1 0 1 2 08 A C1 1 1 5 45 127 70 8 0 10 5 C2 1 2 6 0 127 70 7 0 10 5 C3 1 1 18 30 127 70 20 0 10 5 C4 1 2 19 0 127 70 20 30 10 5 C5 0 1 19 0 127 70 20 30 10 5 C6 0 1 21 0 127 70 22 30 10 5 CS 1 1 127 70 1 30 10 5 CE 0 1 127 70 1 30 10 5 CJ 0 1 127 70 1 30 10 5 0 0 -776 1001000 0 208h 07/12/2022 0 7 1 7 8 251 29 4 0 0 09/09/2023 09/09/2023 22/02/2024 1 4948 2088 0 25 0 425 422 0 freezanz 1 0 9
```

**Field positions (header):**

| Index | Field | Notes |
|-------|-------|-------|
| 0 | `cmd` | Always `G_imp` |
| 1 | `machine_type` | `1`=Evolution, `0`=Portable. This unit is Evolution. |
| 2 | unknown | Always 0 in our captures |
| 3 | `progcic_type` | Probably "program cycle type" |
| 4 | unknown | |
| 5 | `firmware_minor` | `08` here = firmware v2.08 |
| 6 | `secondary_comm_state` | `A` or `B` — used by web UI to track handshake phase |

**Cycle blocks** follow, one per cycle. Each block starts with its label token (`C1`–`C6`, `CS`, `CE`, `CJ`).

**Numbered cycles `C1`–`C6`** — 10 fields each after label:

| Offset | Field | Range | Notes |
|--------|-------|-------|-------|
| +1 | `act` | 0/1 | Disabled/enabled |
| +2 | `mode` | 1/2 | 1=Direct (continuous), 2=Pulse |
| +3 | `start_hour` | 0–23 | |
| +4 | `start_minute` | 0–59 | |
| +5 | `days_bitmap` | 0–127 | 7 bits, one per weekday. 127 = all days. Bit-to-day mapping should be verified empirically by toggling one day in the web UI and observing — likely bit 0 = Monday, bit 6 = Sunday, but not certain. |
| +6 | `duration_seconds` | | Direct mode: total spray duration |
| +7 | `end_hour` | 0–23 | Pulse mode: stop pulsing after this time |
| +8 | `end_minute` | 0–59 | |
| +9 | `work_seconds` | | Pulse mode: spray duration per pulse |
| +10 | `pause_minutes` | | Pulse mode: gap between pulses |

**Special cycles `CS` (Start), `CE` (Extra), `CJ` (Jack)** — 8 fields each (no `start_hour`/`start_minute`):

| Offset | Field |
|--------|-------|
| +1 | `act` |
| +2 | `mode` |
| +3 | `days_bitmap` |
| +4 | `duration_seconds` |
| +5 | `end_hour` |
| +6 | `end_minute` |
| +7 | `work_seconds` |
| +8 | `pause_minutes` |

**Trailing metadata** (~30 fields after CJ block) — most are not needed. Worth extracting:
- Field with value `freezanz` is the WiFi password literal (curiosity, don't expose)
- Date strings like `07/12/2022`, `09/09/2023`, `22/02/2024` are install/service dates
- Field `208h` is firmware string

For v0.1, ignore everything after the CJ cycle block.

**Current configured cycles** (from live capture, for reference):
- C1: Direct, 05:45 → 08:00, 70s spray, all days
- C2: Pulse, 06:00 → 07:00, 70s/10s/5min, all days
- C3: Direct, 18:30 → 20:00, 70s spray, all days
- C4: Pulse, 19:00 → 20:30, 70s/10s/5min, all days
- C5, C6: disabled
- CS: Direct, end 01:30, 70s spray
- CE, CJ: disabled

#### `G_dat` — Live status (push, ~once per `P_dat` sent)

The server sends a `G_dat` in response to each `P_dat` the client sends. Polling rate is therefore controlled by the client.

**Live captured examples:**

Standby state:
```
G_dat 8 0 0 0 0 0 0 0 0 0 0 0 0 354 1 0 1172 1095 0 0 0 1 0 1 0 0 7 5 2026 7 41 1 4 0 12 46 24 7 20 331 0 0 B 1 0 0 0 10 0 300 0 0 0 0 0 0 4 1 3 0
```

Mid-mist state (during a 70-second manual spray, ~3 seconds in):
```
G_dat 4 0 0 0 0 0 0 1 1 1 1 1 0 354 1 0 1172 1095 0 0 0 1 0 1 0 0 7 5 2026 7 45 47 4 0 12 46 24 9 36 333 0 0 B 12 1 1 0 70 0 300 0 200 0 0 0 0 4 1 3 0
```

Just after stop:
```
G_dat 7 0 0 0 0 0 0 1 1 1 1 1 0 354 1 0 1172 1095 0 0 1 1 0 1 0 0 7 5 2026 7 45 47 4 0 12 46 24 6 36 338 0 0 B 12 2 1 1 70 0 300 0 0 0 1 0 0 4 1 3 0
```

**Field positions** (61 total):

| Index | Field | Notes |
|-------|-------|-------|
| 0 | `cmd` | Always `G_dat` |
| 1 | `tick_counter` | Rolls 0–9 each second. Useful as liveness heartbeat. |
| 2 | `mist_done` | Echoes 1 briefly when client's `mist_send` was 1 |
| 3 | `pulse_done` | Echo of pulse_send |
| 4 | `stop_done` | Echo of stop_send |
| 5 | `stopday_done` | Echo of stopday_send |
| 6 | `provapump_done` | Echo of pump test |
| 7 | `provaled_done` | Echo of LED test |
| 8 | `provabuz_done` | Echo of buzzer test |
| 9 | `provaprddue_done` | Echo of product 2 test |
| 10 | `provascar_done` | Echo of discharge test |
| 11 | `prevendita_done` | Echo of presales/demo |
| 12 | `snd_test_done` | Echo of sound test |
| 13 | unknown | Always 0 |
| 14 | sensor_voltage_x10 | NOT a battery — this device is mains powered. Probably an internal sensor reading. **Don't expose as battery.** |
| 15 | unknown | Always 1 |
| 16 | unknown | |
| 17 | `last_winW` | Echoes the winW the client sent in P_dat |
| 18 | `last_winH` | |
| 19–20 | unknown | |
| 21 | `stop_today_state` | 0/1, set after a stop has been issued today |
| 22–26 | unknown / state flags | |
| 27 | `device_day` | 1–31 |
| 28 | `device_month` | 1–12 |
| 29 | `device_year` | e.g. 2026 |
| 30 | `device_hour` | 0–23 |
| 31 | `device_minute` | 0–59 |
| 32 | `device_second` | 0–59 |
| 33 | `device_weekday` | 0–6, our capture showed 4 on Thu 7 May, suggests 0=Sun |
| 34 | unknown | |
| 35 | `machine_type_echo` | |
| 36 | unknown | |
| 37 | unknown | |
| 38 | **`operating_mode`** | **6=stopped, 7=standby, 9=actively misting** ← key state |
| 39 | `substate` | Cycle phase indicator, value changes during pulse |
| 40 | `voltage2_x10` | Another voltage-like reading, varies during operation |
| 41–42 | unknown / always 0 | |
| 43 | `secondary_comm_state` | `A`/`B` |
| 44 | **`active_cycle_id`** | **0=none, 12=manual/Direct override, otherwise C1–C9 mapped (mapping TBD)** |
| 45 | `cycle_state` | 1 when active, 2 right after stop |
| 46 | unknown | |
| 47 | unknown | |
| 48 | **`current_planned_duration_sec`** | **70 during a 70s spray** |
| 49 | unknown / always 0 | |
| 50 | `pause_seconds` | 300 = 5 min, matches pause_min×60 |
| 51 | unknown | |
| 52 | **`elapsed_seconds_in_cycle`** | **Ramps 0→200+ during spray, resets to 0 when stopped** |
| 53 | unknown | |
| 54 | `manual_stop_today_flag` | 1 after a stop command |
| 55–60 | unknown / static | Fields 57–59 = `4 1 3` consistently. May be settings echoes. |

**Critical state derivation:**

- Misting status: `field[38] == 9`
- Standby: `field[38] == 7`
- Stopped: `field[38] == 6`
- Currently active cycle: `field[44]` (0 = none)
- Progress through current spray: `field[52]` / `field[48]` (elapsed / planned, both seconds)

### 3.3 Client → Server message types

#### `P_imp` — Push settings / handshake start

**Minimal handshake form** (what the integration sends right after receiving `CiaO`):

```
P_imp;<weekday_1to7>;<day>;<month>;<year>;<hour>;<minute>;<second>;<lang>;<chksum>;A;0;
```

Where:
- `weekday_1to7` = `datetime.weekday()+1` (Monday=1, Sunday=7) — verify against device weekday after first response
- `chksum` = simple sum: `day + month + year + hour + minute + second`
- `lang` = `"en-US"` (any language code works; device echoes it back somewhere but doesn't seem to validate)
- The literal `A` at position 11 = `seconda_comunic` flag, signals "first communication"
- Trailing `0` = `progcic_type` placeholder

This **must** be sent within a few seconds of receiving `CiaO` or the device may close the connection.

**Verified working** — this exact format triggered a `G_imp` response in our capture session.

**Full settings push form** (used to disable cycles / write schedule):

```
P_imp;<weekday>;<day>;<month>;<year>;<hour>;<minute>;<second>;<lang>;<chksum>;B;<progcic_type>;C1 <act>;<mode>;<start_hr>;<start_min>;<days_bitmap>;<dur_sec>;<end_hr>;<end_min>;<work_sec>;<pause_min>;C2 ...;[...];CJ <act>;<mode>;<days>;<dur>;<end_hr>;<end_min>;<work_sec>;<pause_min>;<vbatcalib_request>;<cicliplus_abilitation>;<change_machinetype_send>;<machine_typevoluto>;<psw_changed>;<psw_tosend>;<lingua_voluta>;<captive_voluto>;<chan_voluto>;<secure_save>;
```

The trailing 10 fields can mostly be 0 / empty:
- `vbatcalib_request` = 0
- `cicliplus_abilitation` = bitmap (1110000 = 1110000 in our config). Computed as: `1000000 if C4 enabled + 100000 if C5 + 10000 if C6 + 1000 if CS + 100 if CE + 10 if CJ + 1 if (cycloP/cycloPi)`
- `change_machinetype_send` = 0
- `machine_typevoluto` = 1 (Evolution)
- `psw_changed` = 0
- `psw_tosend` = `freezanz` (the WiFi password — leave as default)
- `lingua_voluta` = 0
- `captive_voluto` = 0
- `chan_voluto` = 0
- `secure_save` = 0

The literal `B` at position 11 signals "second communication" (settings update, not handshake).

**To disable all cycles**: send a `P_imp` with the current settings but `act=0` for every cycle. Cache the original `G_imp` payload at first observation so it can be restored when the user toggles `switch.zhalt_onboard_scheduler` back on.

#### `P_dat` — Polling / actions

**Format:**

```
P_dat;<winW>;<winH>;<mist_send>;<pulse_send>;<stop_send>;<stopday_send>;<provapump_send>;<provaled_send>;<provabuz_send>;<provaprddue_send>;<provascar_send>;<prevendita_send>;<page_open>;<snd_test_send>;<snd_set_send>;
```

**Constants** the integration should always send:
- `winW` = 1920
- `winH` = 1080
- `page_open` = 1
- `snd_test_send` = 0
- `snd_set_send` = 0

**Action flags** — set to 1 in one outgoing P_dat to trigger, then 0 in subsequent ones. The device's response in `G_dat` fields 2–12 confirms receipt. **Verified working** in live capture:

| Flag position | Action | Verified |
|---------------|--------|----------|
| `mist_send` (3rd) | Start manual mist (uses device's default duration, currently 70s) | ✅ Live test fired the misters |
| `pulse_send` (4th) | Start pulse mode | Not tested but per JS `sendPULSE()` |
| `stop_send` (5th) | Stop active mist immediately | ✅ Live test stopped them |
| `stopday_send` (6th) | Disable today's remaining schedule | Not needed if we disable cycles |
| `provapump_send` (7th) | Diagnostic: pump test | Untested |
| `provaled_send` (8th) | Diagnostic: LED test | Untested |
| `provabuz_send` (9th) | Diagnostic: buzzer test | Untested |
| `provaprddue_send` (10th) | Diagnostic: product 2 (refill?) test | Untested |
| `provascar_send` (11th) | Diagnostic: discharge test | Untested |
| `prevendita_send` (12th) | Demo / presales mode | Untested, leave at 0 |

**Polling cadence:** the web UI sends `P_dat` every ~1.5 seconds in our captures. Use the same interval. Going faster might overload the device (it's a small embedded microcontroller). Going slower means slower state updates.

---

## 4. Architecture

### 4.1 Repo layout

```
ha-zhalt/
├── README.md
├── LICENSE
├── hacs.json
├── test_protocol.py             # standalone protocol exerciser, NO HA imports
├── tests/
│   ├── __init__.py
│   ├── test_protocol_parsing.py # unit tests for G_imp/G_dat parsers
│   └── fixtures/
│       ├── g_imp_sample.txt
│       ├── g_dat_standby.txt
│       └── g_dat_misting.txt
└── custom_components/
    └── zhalt/
        ├── __init__.py
        ├── manifest.json
        ├── const.py
        ├── config_flow.py
        ├── coordinator.py
        ├── protocol.py
        ├── sensor.py
        ├── binary_sensor.py
        ├── button.py
        ├── switch.py
        ├── services.yaml
        ├── strings.json
        └── translations/
            └── en.json
```

### 4.2 Build order

Build in this strict order. Each phase must work end-to-end before the next:

**Phase 1 — `test_protocol.py` (no HA imports)**

Standalone Python script that:
1. Connects to `ws://172.217.28.1:81`
2. Receives `CiaO`
3. Sends handshake `P_imp`
4. Receives `G_imp`, parses it, prints the parsed dict nicely
5. Loops sending `P_dat` every 1.5s, parses each `G_dat` response, prints any field changes
6. After 5 seconds of polling, fires a manual mist (`mist_send=1`)
7. Watches `field[38]` flip from 7→9, `field[52]` start incrementing
8. After 6 seconds of misting, fires stop
9. Confirms `field[38]` returns to 6 then 7
10. Prints "ALL TESTS PASSED" or specific failure

This script is the source of truth for the protocol layer. Once it passes reliably, copy `protocol.py` (the parsing/serialization functions) verbatim into `custom_components/zhalt/`.

**Run it like:**
```bash
cd ha-zhalt
python3 test_protocol.py
```

If working: outputs ~30 seconds of state, fires mist, observes mist, fires stop, exits 0.

**Phase 2 — `custom_components/zhalt/protocol.py`**

Pure Python module with no HA dependencies:
- `parse_g_imp(text: str) -> dict` — returns structured settings dict with all 9 cycles
- `parse_g_dat(text: str) -> dict` — returns structured live state dict
- `build_p_imp_handshake(now: datetime) -> str`
- `build_p_imp_settings(settings: dict, now: datetime) -> str`
- `build_p_dat(actions: dict = {}) -> str` — actions like `{"mist_send": 1}`
- Constants for state mappings (mode 6/7/9 → "Stopped"/"Standby"/"Misting", cycle id 12 → "Manual", etc.)

Unit-test with the fixture files in `tests/fixtures/`.

**Phase 3 — `coordinator.py`**

Subclass `DataUpdateCoordinator`. Push-driven (WebSocket), not pull, but use the coordinator pattern so entities can subscribe.

State machine:
- `DISCONNECTED` → try to connect
- `CONNECTING` → opened WS, waiting for `CiaO`
- `HANDSHAKING` → got `CiaO`, sent `P_imp`, waiting for `G_imp`
- `CONNECTED` → got `G_imp`, polling with `P_dat` every 1.5s, processing `G_dat` responses
- `RECONNECTING` → exponential backoff: 2, 4, 8, 16, 32, 60, 60, 60... seconds

Methods exposed to entities:
- `async fire_action(name: str)` — name in {mist, stop, test_pump, test_led, test_buzzer}
- `async fire_mist_with_duration(seconds: int)` — fires mist, schedules stop after N seconds
- `async write_settings(new_settings: dict)` — sends a P_imp with B mode
- `async disable_all_cycles()` — convenience for the master switch
- `async restore_cycles()` — uses cached original G_imp
- Properties: `data` (current G_dat parsed), `settings` (current G_imp parsed), `connected` (bool)

Background task running the WS receive loop. Another background task firing P_dat keepalives every 1.5s. Both started by `async_setup` and cancelled on `async_unload`.

**Phase 4 — Entity platforms**

`sensor.py`, `binary_sensor.py`, `button.py`, `switch.py`. Standard HA entity boilerplate, all reading from coordinator.data / coordinator.settings.

**Phase 5 — Config flow**

Single-step user form: host (default `172.217.28.1`), port (default `81`). Test the connection by attempting handshake. Show error if no `CiaO` received within 10s.

Add a second step (or option flow) asking: "Take over scheduling?" with options:
- "Not yet — observe only" (default)
- "Yes — disable onboard cycles now"

**Phase 6 — Services**

```yaml
mist:
  name: Mist
  description: Trigger a manual mist for a specified duration.
  fields:
    duration:
      name: Duration
      description: Duration in seconds
      required: true
      example: 70
      selector:
        number:
          min: 5
          max: 300
          unit_of_measurement: seconds
          mode: slider

stop:
  name: Stop
  description: Stop any active mist immediately.

refresh:
  name: Refresh settings
  description: Force re-fetch of settings from device (sends a P_imp handshake).
```

The `mist` service should:
1. Fire `mist_send=1`
2. Wait `duration` seconds
3. Fire `stop_send=1`

This gives HA full control over duration regardless of what the device thinks the default is. Important: if the integration is already misting when called, ignore the call (idempotent) and log a warning.

---

## 5. Entity reference

### Sensors

| Entity ID | Source | Unit | Device class | Notes |
|-----------|--------|------|--------------|-------|
| `sensor.zhalt_state` | `G_dat[38]` mapped | — | enum | `Standby` / `Misting` / `Stopped` / `Unknown` |
| `sensor.zhalt_active_cycle` | `G_dat[44]` mapped | — | enum | `None` / `Manual` / `C1`–`C6` / `Start` / `Extra` / `Jack` |
| `sensor.zhalt_elapsed_seconds` | `G_dat[52]` | s | duration | |
| `sensor.zhalt_planned_duration` | `G_dat[48]` | s | duration | 0 when idle |
| `sensor.zhalt_remaining_seconds` | `planned - elapsed` (clamped ≥0) | s | duration | |
| `sensor.zhalt_firmware_version` | `G_imp[5]` formatted as `2.{value}` | — | — | Diagnostic category |
| `sensor.zhalt_device_clock` | `G_dat[27..32]` composed | — | timestamp | Diagnostic category |
| `sensor.zhalt_machine_type` | `G_imp[1]` | — | enum | `Evolution` / `Portable`. Diagnostic. |
| `sensor.zhalt_tick` | `G_dat[1]` | — | — | Diagnostic, hidden by default |

### Binary sensors

| Entity ID | Logic | Device class |
|-----------|-------|--------------|
| `binary_sensor.zhalt_misting` | `G_dat[38] == 9` | `running` |
| `binary_sensor.zhalt_connected` | WS open AND `G_dat` received within last 10s | `connectivity` |

### Buttons

| Entity ID | Action | Category |
|-----------|--------|----------|
| `button.zhalt_mist_now` | Fires `mist_send=1` (uses device default duration, no auto-stop) | — |
| `button.zhalt_stop` | Fires `stop_send=1` | — |
| `button.zhalt_test_pump` | Fires `provapump_send=1` | diagnostic |
| `button.zhalt_test_led` | Fires `provaled_send=1` | diagnostic |
| `button.zhalt_test_buzzer` | Fires `provabuz_send=1` | diagnostic |

For controlled-duration sprays, users should call the `zhalt.mist` service instead of `button.zhalt_mist_now`.

### Switch

| Entity ID | Behavior |
|-----------|----------|
| `switch.zhalt_onboard_scheduler` | On = restore cached cycles. Off = disable all 9 cycles. State mirrors whether any cycle has `act=1` in current settings. |

### Services

| Service | Purpose |
|---------|---------|
| `zhalt.mist` | Mist for N seconds with auto-stop |
| `zhalt.stop` | Stop immediately |
| `zhalt.refresh` | Force resync settings |

---

## 6. Edge cases & defensive behavior

1. **WebSocket disconnect during a mist** — state goes Unknown, attempt reconnect with backoff. On reconnect, check `G_dat[38]` and reflect actual state (the device keeps misting independently). If a `zhalt.mist` service call was in-flight (waiting to send stop), retry sending stop after reconnect within 30 seconds, otherwise log error.

2. **Multiple rapid `mist` service calls** — second call while already misting is a no-op (log info, return success). Don't queue.

3. **Device clock drift** — log a warning at startup if `G_dat[27..32]` differs from HA's `dt_util.now()` by more than 5 minutes. The device has no NTP and no battery-backed RTC, so it'll drift. Future enhancement: send a `P_imp` periodically just to update its clock.

4. **`G_imp` field count mismatch** — if firmware is updated and adds/removes fields, parser shouldn't crash. Wrap in try/except, log warning with the raw payload, fall back to last-known-good settings.

5. **`G_dat` field count mismatch** — same defensive approach. Don't update `data` if parse fails; entities continue showing last value.

6. **The "1 client at a time" Zhalt limit** — if the WebSocket fails to connect with what looks like a "device busy" symptom (immediate close, no `CiaO`), set `binary_sensor.zhalt_connected` off and emit a persistent notification: "Zhalt is connected to another client. Ensure no devices are joined to ZHALT-EVO_xxxx WiFi directly."

7. **Settings cache for `restore_cycles`** — persist the original `G_imp` to HA's storage (`Store` from `homeassistant.helpers.storage`) the first time we observe one with any cycle enabled. Don't overwrite on subsequent observations (otherwise toggling the master switch off then on would cache the disabled state).

8. **Don't spam `P_imp`** — only send it on (a) initial handshake, (b) explicit settings change, (c) `zhalt.refresh` service call. Settings changes are rare. `P_dat` is the only frequent message.

---

## 7. Manifest

```json
{
  "domain": "zhalt",
  "name": "Zhalt Evolution Connect",
  "version": "0.1.0",
  "documentation": "https://github.com/SimonB33/ha-zhalt",
  "issue_tracker": "https://github.com/SimonB33/ha-zhalt/issues",
  "codeowners": ["@SimonB33"],
  "requirements": ["websockets>=12.0"],
  "iot_class": "local_push",
  "config_flow": true,
  "integration_type": "device"
}
```

`hacs.json` for HACS Custom Repository:

```json
{
  "name": "Zhalt Evolution Connect",
  "render_readme": true,
  "homeassistant": "2024.1.0"
}
```

---

## 8. Testing approach

### Local development loop

1. Develop in a scratch dir on the Mac (or directly via Claude Code).
2. The Mac can already reach the Zhalt at `172.217.28.1` (verified — earlier in this thread).
3. Run `test_protocol.py` against the live device until it passes cleanly.
4. Once protocol layer is solid, build HA wrappers around it.

### Deploying to HA

Two options:

**Option A — File copy via SSH add-on:**
```bash
# From Mac, assuming HA SSH add-on installed and accessible:
rsync -av custom_components/zhalt/ root@10.0.0.9:/config/custom_components/zhalt/
ssh root@10.0.0.9 "ha core restart"
```

**Option B — HACS Custom Repository (proper):**
1. Push to private GitHub repo `SimonB33/ha-zhalt`
2. In HA: HACS → Integrations → ⋮ → Custom Repositories → Add `https://github.com/SimonB33/ha-zhalt` as Integration
3. Install. HA prompts to restart.

For the dev loop, use Option A. For final install, Option B.

### What to verify in HA

After integration is installed and configured:
- `binary_sensor.zhalt_connected` shows `on`
- `sensor.zhalt_state` shows `Standby`
- `sensor.zhalt_firmware_version` shows `2.08`
- `button.zhalt_mist_now` → press → garden mists → `binary_sensor.zhalt_misting` shows `on` within 2 seconds → `sensor.zhalt_state` shows `Misting`
- After ~70s, mist stops naturally → state returns to `Standby`
- Service call: `zhalt.mist` with `duration: 15` → mists for 15 seconds (not 70), then stops
- `switch.zhalt_onboard_scheduler` toggle off → device's onboard cycles stop firing (verify next day at 05:45 — no spray)
- Toggle back on → cycles restored, next morning sprays again

### Pre-built sample automations for the README

```yaml
# Dawn spray
automation:
  - alias: "Mosquitos: Dawn spray"
    trigger:
      platform: sun
      event: sunrise
      offset: "-00:30:00"
    condition:
      - condition: state
        entity_id: input_boolean.zhalt_disabled_today
        state: "off"
      - condition: template
        value_template: "{{ now().month not in [6,7,8] }}"
    action:
      - service: zhalt.mist
        data:
          duration: 70

# Reset the manual override at midnight
  - alias: "Mosquitos: Reset daily override"
    trigger:
      platform: time
      at: "00:00:00"
    action:
      - service: input_boolean.turn_off
        entity_id: input_boolean.zhalt_disabled_today
```

---

## 9. Out of scope for v0.1

These can wait for v0.2+:
- Editing individual cycle schedules from HA UI (currently we just disable them all)
- Updating device clock from HA
- Multi-language support beyond English
- WebSocket TLS (device doesn't support it anyway)
- Device discovery (no mDNS / SSDP from this device, so nothing to discover)
- Calibration / dosage settings (the `vbatcalib_request` and dosage tables in the original HTML)

---

## 10. Reference: original HTML JS for protocol context

The full Zhalt HTML (`zhalt.html`, ~96KB compressed, ~525KB uncompressed) is available if needed — the relevant JS functions are:

- `OpenWebsocket()` — defines `ws_urlIP="ws://172.217.28.1:81"`
- `ws.onmessage` — handles `CiaO`, `G_imp`, `G_dat`
- `post_impo()` — generates the `P_imp` string (this is the canonical reference for field order in `P_imp;B;...`)
- `post_dati()` — generates the `P_dat` string
- `sendMIST()`, `sendPULSE()`, `sendSTOP()`, `sendSTOPday()` — set the relevant `*_send` window globals to 1
- `get_impo()` and `get_dati()` — parse the responses (canonical reference for field meanings)

If anything in this spec is ambiguous, those functions in the HTML are the source of truth. The integration should match their behavior.

---

## 11. Initial commit plan for Claude Code

1. Create empty repo `SimonB33/ha-zhalt`, push initial README + LICENSE (MIT)
2. Build `test_protocol.py`, run against live device, iterate until passing
3. Extract `protocol.py` from `test_protocol.py`
4. Add unit tests with fixtures
5. Build `coordinator.py`, deploy to HA via Option A, verify connection works
6. Add `binary_sensor.py` (just connected + misting), verify in HA
7. Add `sensor.py`, verify
8. Add `button.py` (mist_now, stop), verify mist works end-to-end from HA
9. Add `services.yaml` and service handlers for `mist` (with duration), verify
10. Add `switch.py` (onboard_scheduler) and full settings push, verify by toggling and observing next scheduled cycle skipped
11. Add diagnostic test buttons
12. Polish: `manifest.json`, `hacs.json`, `strings.json`, translations, README with sample automations
13. Tag v0.1.0, push to GitHub, install via HACS Custom Repository, full smoke test
