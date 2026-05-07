# Zhalt — next steps when you're back

## Current state (2026-05-07)

- **Shipped on `main`, tagged `v0.1.0`**: full integration deployed via HACS
  Custom Repository (`SimonB33/ha-zhalt`, public).
- 17 entities live: 2 binary_sensor, 8 visible sensors (+ tick hidden),
  5 buttons, 1 switch, 1 update.
- 3 services: `zhalt.mist {duration}`, `zhalt.stop`, `zhalt.refresh`.
- Mobile dashboard at `/zhalt-mobile` (sidebar entry "Zhalt"), confirms on
  every mist trigger.
- **On-demand session model in place** (post-v0.1.0): WS opens only on action,
  closes after action-specific hold. 30 s health check every 30 min between
  05:00–23:00 local so HA notices when the device returns from a power cycle.
- **Onboard scheduler is ON** — device's own cycles still drive the spray.

## Pending — needs your input

### 1. Decide on HA-managed automations vs onboard scheduler

We're currently mixed (onboard scheduler on, no HA automations). To go fully
HA-managed, build the automations first, then turn the scheduler off.

Options for review:

- **Automation 1 — Dawn spray** (most useful)
  - Trigger: sunrise, offset −30 min
  - Conditions: `input_boolean.zhalt_disabled_today` off; month not in Jun/Jul/Aug
  - Action: `zhalt.mist` with `duration: 70`
- **Automation 2 — Reset daily override**
  - Trigger: 00:00:00
  - Action: turn off `input_boolean.zhalt_disabled_today`
- **Automation 3 — Dusk spray** (optional)
  - Trigger: sunset, offset +30 min
  - Same conditions, same action

Prerequisite: create helper `input_boolean.zhalt_disabled_today`
(Settings → Devices & Services → Helpers → Toggle).

**Decisions needed:**
- Which of {1, 2, 3} to build?
- Keep the summer skip (Jun/Jul/Aug) or invert it?
- Mist duration (default 70 s)?
- After they're in place, flip onboard scheduler off?

### 2. Smoke tests deferred (need owner approval; require water-on-skin awareness)

- Mist round-trip via `button.zhalt_mosquito_mist_now` — expect state goes
  `Misting`, returns to `Standby` after ~70 s.
- Timed mist via `zhalt.mist {duration: 15}` — expect 15 s spray, not 70.
- Scheduler off → on round-trip — verify cached cycle restore (small risk if
  cache is wrong, fixable via device's HTML UI).

### 3. Open observation

- During the previous deploy session, the Zhalt unit went offline (Opal lost
  Wi-Fi association to `ZHALT-EVO_61B3z`, then unit was unreachable on Wi-Fi
  at all). User power-cycled / reset and it's back. The new on-demand session
  model is partly a hedge against this happening from sustained 24/7 client
  load on a device designed for occasional captive-portal access. Watch for
  recurrence.

## Ground rules (durable)

- **No mist tests without explicit owner approval.** Garden may have people.
- After any manual stop, device sets `stop_today_state=1` and aborts manual
  mists for the rest of the day. Expected, not a bug. Scheduled cycles next
  day are unaffected.

## Useful pointers

- Spec & protocol reference: `zhalt-ha-integration-spec.md`
- Live entity check: `ha_search_entities(query="zhalt")`
- Force a session: dashboard → "Refresh settings" or call `zhalt.refresh`
- Dashboard URL: `/zhalt-mobile/mosquito`
