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
# Periodic health-check session: brings HA back in sync after the device returns
# from a power cycle / reboot, without the cost of a 24/7 keepalive.
HEALTHCHECK_INTERVAL_S = 30 * 60
HEALTHCHECK_HOLD_S = 30.0
HEALTHCHECK_WINDOW_HOURS: tuple[int, int] = (5, 23)  # 05:00 inclusive, 23:00 exclusive (HA local time)
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

ATTR_DURATION = "duration"
