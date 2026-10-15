# Zhalt HA Cut-over (plan 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move every Zhalt script, automation, safety and dashboard in Home Assistant from the Opal + HACS integration onto the ESP32 bridge's MQTT interface, and correct the spray schedule now that sessions start in seconds.

**Architecture:** HA stays the scheduler and the gatekeeper (wind, rain, doors, people, skips). The scripts publish JSON commands to `zhalt/cmd` and watch the board's `zhalt_mqtt_*` entities. The board enforces caps and blackout windows, which HA keeps in sync through the retained `zhalt/config`. The Shelly stays the hardware kill, and it gains a power backstop for sprays the board did not account for.

**Tech Stack:** Home Assistant (UI-managed scripts, automations, helpers and dashboards via the HA MCP tools), MQTT (Mosquitto add-on), ESP32 firmware on branch `esp32-bridge` (ac16e99).

**Spec:** `docs/superpowers/specs/2026-10-15-esp32-bridge-design.md` §5 (HA changes) and §6.4–6.5 (supervised session, cut-over).

## Global Constraints

- **No spraying without Simon's explicit OK for that specific test.** Every verification step in Tasks 1–6 must be non-spraying (a gate that trips, a skip path, a refresh, a template render).
- **All five spray automations stay disabled** (`car_wash_morning_mist`, `morning_mist`, `evening_blast`, `morning_pulse_session`, `evening_pulse_session`) until Simon says to re-enable them. Edit them, never enable them.
- **Before every change, save the object's current config verbatim** (re-fetch it with the MCP get tool; the plan2-context dumps are condensed) to `.superpowers/sdd/plan2-context/backup/<entity_id>.json`. That backup is the rollback.
- Script and helper entity IDs keep their names, so dashboard buttons that call them keep working.
- The board's limits are fixed: mist 5–120 s, pulse 10–90 min, `resume_min_minutes` 0–90. HA must not send values outside them. Clamp in templates.
- `switch.usw_pro_max_16_poe_port_6_poe` now powers the ESP32. **Nothing may turn it off** except an explicit "Reboot bridge (power)" action.
- MQTT command payloads must match `firmware/lib/zcore/src/zconfig.cpp` exactly:
  - `{"action":"mist","seconds":N}`
  - `{"action":"pulse","minutes":N}`
  - `{"action":"stop"}`
  - `{"action":"refresh"}`
  - `{"action":"scheduler","on":true|false}`
- `zhalt/config` (retained): `{"mist_seconds":N,"resume_min_minutes":N,"blackouts":[{"start":"HH:MM","end":"HH:MM"}, ×3]}`.
- Board state values: `idle`, `misting`, `pulse_session`, `paused_blackout`, `stopping`, `stop_failed`.
- Notifications go to `notify.mobile_app_simons_iphone_new`.

## Review Focus

1. **A spray that HA did not start:** a dashboard MQTT button, the unit's own cycle, or a pulse session that ignored Stop. Expected: the Shelly backstop cuts mains if the board is not in a spraying state while the pump draws more than 100 W (Task 3).
2. **The bridge is offline when a scheduled spray fires.** Expected: the script waits at most 30 s for the board to acknowledge, logs a skip with a reason, and notifies. It never leaves the timer running or the Shelly off.
3. **A person or door trips during a pulse session that a dashboard button started**, so HA's timer was never started. Expected: the safety abort and the emergency cut still fire, because the gate is the board's state rather than only the timer (Task 3).
4. **A blackout window starts during a pulse session.** Expected: the board pauses at the window start and resumes at the window end if at least 10 min remain. HA must not fight that by sending a Stop or retrying (Task 2 config, Task 4 schedule).
5. **HA restarts mid-session.** Expected: the retained `zhalt/config` is republished, the board keeps its own session timer, and the scripts in `mode: single` don't double-fire.

---

### Task 1: Repoint the three scripts to MQTT

**Files (HA objects):**
- Modify: `script.zhalt_safe_mist`, `script.zhalt_safe_pulse_session`, `script.zhalt_safe_toggle_scheduler`
- Backup: `.superpowers/sdd/plan2-context/backup/script.*.json`

**Interfaces:**
- Consumes: `zhalt/cmd`; `sensor.zhalt_mqtt_state`; `sensor.zhalt_mqtt_last_result`; `binary_sensor.zhalt_mqtt_online`; `sensor.shelly_zhalt_power`
- Produces: the same script IDs and fields as today (`duration`, `announce_message`, `override_block` for mist; `pulse_minutes`, `announce_message` for pulse; the existing toggle fields), so callers don't change.

- [ ] **Step 1:** Back up all three scripts verbatim.
- [ ] **Step 2:** Rewrite `zhalt_safe_mist`. Keep from the old script:
  - the Shelly turn-on
  - the GATES block
  - the 30 s announce and the re-gate
  - the `timer.zhalt_spray_window` start (2 min)
  - MARK_SKIP and the notifications

  Delete every PoE, ping, 135 s buffer and POE_OFF step. Replace the fire-and-verify section with:

```yaml
- alias: Bridge must be online
  if: [{condition: state, entity_id: binary_sensor.zhalt_mqtt_online, state: "off"}]
  then:
    - action: input_datetime.set_datetime
      target: {entity_id: input_datetime.zhalt_last_skip_at}
      data: {timestamp: "{{ now().timestamp() | int }}"}
    - action: notify.mobile_app_simons_iphone_new
      data: {title: "Zhalt mist FAILED", message: "ESP32 bridge offline. No spray.", data: {tag: zhalt-fail, interruption-level: critical}}
    - stop: "Bridge offline"
- action: mqtt.publish
  data:
    topic: zhalt/cmd
    payload: >-
      {"action":"mist","seconds":{{ [[duration | default(70) | int, 5] | max, 120] | min }}}
- alias: Wait for the board to report misting
  wait_template: "{{ is_state('sensor.zhalt_mqtt_state', 'misting') }}"
  timeout: {seconds: 30}
  continue_on_timeout: true
- if: [{condition: template, value_template: "{{ not wait.completed }}"}]
  then:
    - action: input_datetime.set_datetime
      target: {entity_id: input_datetime.zhalt_last_skip_at}
      data: {timestamp: "{{ now().timestamp() | int }}"}
    - action: notify.mobile_app_simons_iphone_new
      data:
        title: "Zhalt mist FAILED"
        message: "Board did not start: {{ states('sensor.zhalt_mqtt_last_result') }}"
        data: {tag: zhalt-fail, interruption-level: critical}
    - stop: "Board did not start"
- alias: Confirm pump draw (ground truth)
  wait_for_trigger: [{trigger: numeric_state, entity_id: sensor.shelly_zhalt_power, above: 50}]
  timeout: {seconds: 25}
  continue_on_timeout: true
- if: [{condition: template, value_template: "{{ not wait.completed }}"}]
  then:
    - action: notify.mobile_app_simons_iphone_new
      data: {title: "Zhalt mist: no pump draw", message: "Board says misting but Shelly saw no draw >50 W.", data: {tag: zhalt-fail}}
- alias: Wait for the board to finish
  wait_template: "{{ not is_state('sensor.zhalt_mqtt_state', 'misting') }}"
  timeout: {seconds: 150}
  continue_on_timeout: true
```

- [ ] **Step 3:** Rewrite `zhalt_safe_pulse_session` the same way:
  - Publish `{"action":"pulse","minutes":{{ [[pulse_minutes|int,10]|max,90]|min }}}`.
  - Wait 30 s for state `pulse_session`.
  - Start `timer.zhalt_spray_window` for `pulse_minutes + 5` min.
  - Confirm the first burst: Shelly above 50 W within 6 min. If it doesn't come, notify; don't stop.
  - No PoE-off.
- [ ] **Step 4:** Rewrite `zhalt_safe_toggle_scheduler` to publish `{"action":"scheduler","on":<bool>}` and wait 30 s for `switch.zhalt_mqtt_onboard_scheduler` to match.
- [ ] **Step 5: Verify without spraying.**
  - Temporarily set `input_datetime.zhalt_blackout_evening_start`/`_end` around the current time, so that `binary_sensor.zhalt_spray_block` turns on. Then run `script.zhalt_safe_mist`.
  - Expected: a "Mist not fired" notification, no publish on `zhalt/cmd` (the board log shows no `command` line), `last_skip_at` = now, and the Shelly at 0 W.
  - Restore the two helpers (Task 2 republishes the config).
  - Read all three scripts back and search them for `usw_pro_max_16_poe_port_6_poe`, `opal`, `zhalt.mist` and `zhalt_mosquito`. Expected: no matches.
- [ ] **Step 6:** Ledger entry with the backup paths.

### Task 2: Publish the board config from the helpers

**Files (HA objects):**
- Create: `input_number.zhalt_blackout_resume_min_minutes` (min 0, max 90, step 1, initial 10, mode box)
- Create: `automation.zhalt_publish_bridge_config`

**Interfaces:**
- Consumes: `input_number.zhalt_long_blast_seconds`, `input_datetime.zhalt_blackout_{morning1,morning2,evening}_{start,end}`, the new helper, `binary_sensor.zhalt_mqtt_online`
- Produces: the retained `zhalt/config`

- [ ] **Step 1:** Create the helper.
- [ ] **Step 2:** Create the automation:

```yaml
alias: Zhalt publish bridge config
description: Keeps the ESP32 bridge's retained zhalt/config in sync with the dashboard helpers.
mode: restart
triggers:
  - {trigger: homeassistant, event: start}
  - trigger: state
    entity_id:
      - input_number.zhalt_long_blast_seconds
      - input_number.zhalt_blackout_resume_min_minutes
      - input_datetime.zhalt_blackout_morning1_start
      - input_datetime.zhalt_blackout_morning1_end
      - input_datetime.zhalt_blackout_morning2_start
      - input_datetime.zhalt_blackout_morning2_end
      - input_datetime.zhalt_blackout_evening_start
      - input_datetime.zhalt_blackout_evening_end
  - {trigger: state, entity_id: binary_sensor.zhalt_mqtt_online, to: "on"}
actions:
  - delay: {seconds: 2}
  - action: mqtt.publish
    data:
      topic: zhalt/config
      retain: true
      payload: >-
        {% set w = ['morning1','morning2','evening'] %}
        {"mist_seconds":{{ [[states('input_number.zhalt_long_blast_seconds')|int(70),5]|max,120]|min }},
         "resume_min_minutes":{{ [[states('input_number.zhalt_blackout_resume_min_minutes')|int(10),0]|max,90]|min }},
         "blackouts":[{% for n in w %}{"start":"{{ states('input_datetime.zhalt_blackout_' ~ n ~ '_start')[:5] }}","end":"{{ states('input_datetime.zhalt_blackout_' ~ n ~ '_end')[:5] }}"}{{ ',' if not loop.last }}{% endfor %}]}
```

- [ ] **Step 3: Verify.**
  - Run the automation.
  - Then `curl -s http://10.0.0.30/status.json` (or the status page) plus the board log. Expected: no `config rejected` line.
  - Render the payload with `ha_eval_template` and check that it is valid JSON with three windows: 05:55–06:15, 07:00–07:20 and 17:30–18:30.
  - Change `zhalt_blackout_resume_min_minutes` to 11, confirm the automation fired (trace), and set it back to 10.

### Task 3: Safeties

**Files (HA objects):**
- Modify: `automation.zhalt_emergency_power_cut`, `automation.zhalt_safety_abort`, `automation.zhalt_notify_on_runaway_spray`
- Create: `automation.zhalt_shelly_backstop`, `automation.zhalt_bridge_offline_alert`, `automation.zhalt_no_mist_alert`

**Interfaces:**
- Consumes: `sensor.zhalt_mqtt_state`, `sensor.shelly_zhalt_power`, `binary_sensor.zhalt_mqtt_online`, `binary_sensor.zhalt_mqtt_zhalt_reachable`, `sensor.zhalt_mqtt_last_result`
- Produces: the shared "spraying" condition used below, `{{ states('sensor.zhalt_mqtt_state') in ['misting','pulse_session','paused_blackout','stopping'] }}`, abbreviated **SPRAYING**.

- [ ] **Step 1:** Back up the three existing automations.
- [ ] **Step 2: `zhalt_safety_abort`.**
  - Gate: `timer.zhalt_spray_window` active **or** SPRAYING.
  - Action: keep `timer.finish`, and replace `zhalt.stop` with `mqtt.publish zhalt/cmd {"action":"stop"}`.
- [ ] **Step 3: `zhalt_emergency_power_cut`.**
  - Same gate change (timer active or SPRAYING).
  - Add trigger `sensor.zhalt_mqtt_state` → `stop_failed` (id `stop_failed`), which always cuts, like the emergency button.
  - Keep the 5-min re-arm.
  - Remove `script.zhalt_safe_*` from `script.turn_off` only if it errors on missing scripts (it doesn't), so leave it.
  - Fix the description's Opal wording.
- [ ] **Step 4:** Create `zhalt_shelly_backstop`, which closes the open "Stop during pulse pause" gap:

```yaml
alias: Zhalt Shelly backstop
description: Pump drawing while the bridge says nothing is spraying = unaccounted spray (ignored Stop, unit's own cycle, unknown). Cut mains.
mode: single
triggers:
  - trigger: numeric_state
    entity_id: sensor.shelly_zhalt_power
    above: 100
    for: {seconds: 5}
conditions:
  - condition: template
    value_template: >-
      {{ states('sensor.zhalt_mqtt_state') in ['idle','stop_failed'] and
         (as_timestamp(now()) - as_timestamp(states('sensor.zhalt_mqtt_last_mist'), 0)) > 20 }}
actions:
  - action: switch.turn_off
    target: {entity_id: switch.shelly_zhalt}
  - action: notify.mobile_app_simons_iphone_new
    data:
      title: "Zhalt BACKSTOP cut"
      message: "Pump drew {{ states('sensor.shelly_zhalt_power') }} W while the bridge was {{ states('sensor.zhalt_mqtt_state') }}. Mains cut; re-arming in 5 min."
      data: {tag: zhalt-emergency, interruption-level: critical}
  - delay: {minutes: 5}
  - action: switch.turn_on
    target: {entity_id: switch.shelly_zhalt}
```

  Tolerance: the board flips to `idle` when the mist ends, and the pump tails off within about 2 s (seen at 20:00:51). The 20 s after `last_mist` absorbs that.

- [ ] **Step 5:** Create `zhalt_bridge_offline_alert`:
  - Triggers: `binary_sensor.zhalt_mqtt_online` off for 10 min, or `sensor.zhalt_mqtt_join_failures_24h` above 1. Confirm the exact entity ID from `mqtt-entities.md` before writing it.
  - Action: notify, with the last result and the Wi-Fi signal.
- [ ] **Step 6:** Create `zhalt_no_mist_alert`:
  - Daily at 09:00.
  - Conditions: any of the five spray automations is on, `binary_sensor.zhalt_skip_active` is off, and `input_datetime.zhalt_last_power_draw` is more than 48 h old.
  - Action: notify.
- [ ] **Step 7:** Repoint `zhalt_notify_on_runaway_spray`:
  - Trigger: `sensor.zhalt_mqtt_state` → `stop_failed`, replacing the old integration event.
  - Message: "Board Stop failed 3× — mains cut by emergency automation."
- [ ] **Step 8: Verify without spraying.**
  - Render every SPRAYING and backstop template with `ha_eval_template`. Expected: false now (board idle).
  - Press `input_button.zhalt_emergency_stop` while the unit is idle. Expected: the Shelly goes off, then is back on after 5 min, and you get the notification. **Tell Simon first.** This takes the unit off mains for 5 min; it doesn't spray.
  - Read all six automations back and check them for `zhalt.stop`, `zhalt_mosquito_` and `opal`. Expected: none.

### Task 4: Schedule: remove the cold-boot lead

**Proposal (for Simon's approval in review; the automations stay disabled either way):**

| Spray | Days | Now: trigger → real spray | Proposed trigger = real spray | Length |
|---|---|---|---|---|
| Car-wash mist | Tue, Sat | 05:46 → 05:50 | **05:50** | 70 s (`zhalt_long_blast_seconds`) |
| Morning pulse session | Mon, Wed, Thu, Fri, Sun | sunrise−34 → sunrise−30 (05:47 today) | **sunrise−30** | **40 min** (now 60). The board pauses 05:55–06:15 and resumes, ending about 06:47, before the 06:55 mist. With 60 min it ran into the 06:55 mist and the 07:00 blackout. |
| Morning mist | Mon, Wed, Thu, Fri, Sun | 06:51 → 06:55 | **06:55** | 70 s |
| Evening mist | daily | 18:26 → 18:30 | **18:30** (the evening blackout ends 18:30, exclusive) | 70 s |
| Evening pulse session | daily | 18:56 → 19:00 | **19:00** | 90 min (the board's max) |

**Files (HA objects):**
- Modify: `automation.zhalt_car_wash_morning_mist` (05:50), `automation.zhalt_morning_mist` (06:55), `automation.zhalt_evening_blast` (18:30)
- Modify: `automation.zhalt_pulse_times_sun_track`:
  - morning = sunrise − 30, still pushed past a blackout end + 5 min
  - evening = 19:00
  - delete the four `opal_*` writes
  - failure check = pulse start + 10 min
- Modify: the misting-cooldown condition in `morning_mist` and `evening_blast`. Change `binary_sensor.zhalt_mosquito_misting` to `binary_sensor.zhalt_mqtt_misting`, and also skip if `sensor.zhalt_mqtt_state` is in `['pulse_session','paused_blackout']`.
- Modify: `input_number.zhalt_morning_pulse_minutes` → 40
- Modify: the `sensor.zhalt_next_spray` template:
  - fixed times 05:50, 06:55 and 18:30
  - pulse times with no +4
  - skip candidates whose automation is disabled. If all are disabled, the state is `unknown`, so the dashboard shows "—" rather than a fake time.
- Modify: `automation.zhalt_failure_detection_cycle_shelly`:
  - fixed triggers 05:55 → 05:56 (car wash), 07:00 → 07:21 (after the morning-2 blackout), 18:35 (evening mist) and 19:05 → 19:10 (evening pulse)
  - before the retry `script.zhalt_safe_mist`, add a wait of up to 30 min for `binary_sensor.zhalt_blackout_active` off
- Delete: `automation.zhalt_reload_integration_before_sessions_self_heal`
- Delete: helpers `input_datetime.zhalt_opal_{morning,evening}_{on,off}`, but only after a search shows no remaining references.

- [ ] **Step 1:** Back up everything listed.
- [ ] **Step 2:** Apply the edits. **Don't enable** any spray automation.
- [ ] **Step 3: Verify.**
  - Run `zhalt_pulse_times_sun_track` (it only writes helpers). Expected: `zhalt_morning_pulse_start` = today's sunrise − 30 min; `zhalt_evening_pulse_start` 19:00.
  - Render `sensor.zhalt_next_spray`. Expected `unknown`, since every spray automation is disabled.
  - Then render the template body with the disabled-filter bypassed. Expected: tomorrow's earliest proposed time.
  - Search HA for `opal`, `zhalt_mosquito_`, `zhalt.mist`, `zhalt.stop`, `zhalt.refresh` and `port_6_poe`. Expected: only dashboards, which Task 5 fixes, and the YAML `binary_sensor.zhalt_opal_ping` (Task 7).

### Task 5: Dashboards

**Files:** `zhalt-mobile`, `office-ipad` view `mosquito`, `kitchen-ipad-new` views `mosquito` and Home. The reference list is in `.superpowers/sdd/plan2-context/dashboards/zhalt-refs.md`.

- [ ] **Step 1:** Back up each dashboard config verbatim, with its `config_hash`.
- [ ] **Step 2:** Swap entities following spec §5's mapping table:
  - `zhalt_mosquito_*` → `zhalt_mqtt_*`
  - `zhalt.refresh` → `button.zhalt_mqtt_refresh`
  - elapsed/remaining → "Session ends at" + "Last mist"
  - Opal ping/connected → Bridge online + Zhalt reachable
  - the "PoE strategy" / "Opal PoE auto-schedule" markdown → one bridge status line: online, RSSI, last contact, last result
  - drop `input_number.zhalt_pulse_interval_minutes`, which doesn't exist
- [ ] **Step 3:** Add:
  - a Start pulse session tile (calls `script.zhalt_safe_pulse_session` with the morning minutes) next to Mist now
  - a collapsed Bridge section: Wi-Fi signal, join failures 24 h, Test join, Reboot
  - an EMERGENCY CUT tile on office-ipad, which lacks one
- [ ] **Step 4: Verify.**
  - Fetch each dashboard back and search for `zhalt_mosquito_`, `opal` and `zhalt.`. Expected: none.
  - Open `zhalt-mobile` in the browser and check for no red "entity not available" cards.

### Task 6: Supervised session (needs Simon's explicit OK at the time, garden clear)

- [ ] **Step 1:** 15 s mist via `script.zhalt_safe_mist` (`duration: 15`). Expected:
  - board `misting` → `idle`
  - Shelly above 400 W for about 15 s
  - the board's backstop Stop logged as unneeded or ok
- [ ] **Step 2:** Pulse session via `script.zhalt_safe_pulse_session` (`pulse_minutes: 10`). Expected:
  - first burst within about 6 min
  - the board rejoins at 10 min and stops
  - **capture `/status.json` and the board log during a between-burst pause**: the pause-phase `G_dat`, which is the open spec question
- [ ] **Step 3:** Stop during a pause: start another 10-min session, press Stop between bursts. Expected: no further bursts for 15 min (Shelly). If one comes, the Task 3 backstop must cut it within about 5 s. Record which happened.
- [ ] **Step 4:** Ledger: Shelly power history for the whole window and the board log.

### Task 7: Cut-over clean-up

- [ ] **Step 1:** Disable the HACS config entry `01KR0G1KVGZPGBMKGFQJNTM7MG` (Zhalt Evolution Connect). Leave it installed.
- [ ] **Step 2:** Remove the UDM static route 172.217.28.0/24 → 10.0.0.30. **Simon does this in the UniFi UI**; the API route was refused before.
- [ ] **Step 3:** Remove the YAML `binary_sensor.zhalt_opal_ping` (and `zhalt_opal_http` if present) from the HA config files. **Simon or an approved ssh session does this**; MCP can't read YAML.
- [ ] **Step 4:** Re-run the Task 4 search. Expected: no references to the old integration anywhere.
- [ ] **Step 5:** Update memory: `zhalt-mosquito-system.md`, the RESUME section.
- [ ] **Step 6:** The spray automations stay **off**. Tell Simon they're ready to re-enable when he says so.
