# Zhalt Evolution Connect — Home Assistant integration

Local-push integration for the **Freezanz Zhalt Evolution Connect** outdoor-mosquito misting system. Talks to the device's onboard WebSocket directly — no cloud, no app required.

## Features

- Live state: `Stopped` / `Standby` / `Misting`, active cycle, elapsed / remaining / planned duration
- Connectivity sensor (with 10s freshness check)
- Buttons: mist now, stop, plus diagnostic test_pump / test_led / test_buzzer
- Services: `zhalt.mist` (timed mist with auto-stop), `zhalt.stop`, `zhalt.refresh`
- Master switch: enable / disable all 9 onboard scheduler cycles in one toggle, with original-cycle restore
- Diagnostics: firmware version, machine type, device clock

## Installation (HACS — custom repository)

1. HACS → Integrations → ⋮ → Custom Repositories
2. Add `https://github.com/SimonB33/ha-zhalt` as **Integration**
3. Install, restart Home Assistant
4. Settings → Devices & Services → Add Integration → **Zhalt Evolution Connect**
5. Enter the device host (default `172.217.28.1`) and port (default `81`)

The integration confirms the connection by exchanging the device's `CiaO` greeting and waiting for the first `G_dat` frame within 10 s.

## Network notes

The Zhalt's onboard AP is `ZHALT-EVO_xxxx` (open WiFi, no password). Only **one** WebSocket client at a time is allowed. If you're routing through a travel router (e.g. GL.iNet Opal) so HA on your main LAN can reach the device, make sure no other device (phone, browser tab) is also connected — the second client will get pushed off and reconnects loop.

## Sample automations

```yaml
# Dawn spray (30 min before sunrise, off in summer when AC is preferred)
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

## Device behavior worth knowing

- **`stop_today_state` lockout** — after any manual stop the device blocks subsequent manual mists for the rest of the day (resets next day). Scheduled cycles are not affected.
- **Device clock has no NTP** — it drifts. The `Device clock` sensor exposes the current value; a warning is logged at startup if it differs from HA by more than 5 minutes.
- **One client at a time** — see network notes above.

## Services

| Service | Fields | Purpose |
|---------|--------|---------|
| `zhalt.mist` | `duration` (1–120 s) | Mist for N seconds, then auto-stop. No-op if already misting. |
| `zhalt.stop` | — | Stop misting now. |
| `zhalt.refresh` | — | Force a fresh handshake to re-pull onboard settings. |

## Development

Protocol layer is in `custom_components/zhalt/protocol.py` with unit tests in `tests/test_protocol_parsing.py` and a live integration script `test_protocol.py` (point at your device's WebSocket URL).

```bash
.venv/bin/python -m unittest tests.test_protocol_parsing
```

## License

MIT — see [LICENSE](LICENSE).
