"""Domain-level constants for the Zhalt integration."""
from __future__ import annotations

DOMAIN = "zhalt"
PLATFORMS: list[str] = ["binary_sensor", "button", "sensor", "switch"]

DEFAULT_HOST = "172.217.28.1"
DEFAULT_PORT = 81
DEFAULT_NAME = "Zhalt Evolution Connect"

POLL_INTERVAL_S = 1.5
HANDSHAKE_TIMEOUT_S = 10.0
WS_RECV_TIMEOUT_S = 10.0
DAT_STALE_AFTER_S = 10.0  # binary_sensor.zhalt_connected goes off after this gap
RECONNECT_BACKOFF_S: tuple[float, ...] = (2, 4, 8, 16, 32, 60, 60, 60)
CLOCK_DRIFT_WARN_S = 300

# On-demand session model: open WS only when needed, hold open for hold_seconds,
# then close. Hold values balance "see the action's effect" vs "spare the device".
STARTUP_HOLD_S = 30.0
SETTINGS_HOLD_S = 10.0
REFRESH_HOLD_S = 30.0
ACTION_HOLD_DEFAULT_S = 5.0

# Self-heal: if this many session_loop invocations end without ever reaching
# CONNECTED, the coordinator schedules an integration reload. RELOAD_COOLDOWN_S
# is the minimum gap between such reloads so we don't spin against a dead device.
# Sessions only run from user/automation actions now (no healthcheck loop),
# so the threshold is hit only when scheduled sprays / manual presses repeatedly
# fail to reach CONNECTED.
MAX_CONSECUTIVE_FAILURES = 3
RELOAD_COOLDOWN_S = 30 * 60

# Auto-stop retry schedule. If stop_send fails the device keeps misting in
# Manual mode (no onboard timeout), so we retry aggressively with backoff.
# Total window must fit inside the mist session hold (see fire_mist_with_duration).
STOP_RETRY_BACKOFF_S: tuple[float, ...] = (0.0, 1.0, 2.0, 4.0, 8.0, 16.0)
STOP_RETRY_TOTAL_S: float = sum(STOP_RETRY_BACKOFF_S)  # 31.0s

# Initial-mist session-establish retry schedule. If the device is briefly
# unreachable (Wi-Fi flap / DHCP renewal / bridge blip) the user sees an
# immediate "device unreachable" error. Retrying a few times with short
# backoff lets transient outages recover before surfacing the failure.
# Each attempt itself can take up to HANDSHAKE_TIMEOUT_S*2 = ~20s.
SESSION_ESTABLISH_RETRY_BACKOFF_S: tuple[float, ...] = (2.0, 5.0, 10.0)
SESSION_ESTABLISH_RETRY_TOTAL_S: float = sum(SESSION_ESTABLISH_RETRY_BACKOFF_S)  # 17.0s
ACTION_HOLD_S: dict[str, float] = {
    "mist_send": 75.0,        # default device mist is ~70s; cover + small buffer
    "pulse_send": 75.0,
    "stop_send": 5.0,
    "stopday_send": 5.0,
    "provapump_send": 10.0,
    "provaled_send": 5.0,
    "provabuz_send": 5.0,
    "provaprddue_send": 5.0,
    "provascar_send": 5.0,
    "prevendita_send": 5.0,
}

STORAGE_VERSION = 1
STORAGE_KEY = f"{DOMAIN}_settings_cache"

CONF_HOST = "host"
CONF_PORT = "port"

# Service names
SERVICE_MIST = "mist"
SERVICE_STOP = "stop"
SERVICE_REFRESH = "refresh"
SERVICE_DISABLE_CYCLES = "disable_all_cycles"
SERVICE_RESTORE_CYCLES = "restore_cycles"

ATTR_DURATION = "duration"
