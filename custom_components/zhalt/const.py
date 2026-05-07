"""Domain-level constants for the Zhalt integration."""
from __future__ import annotations

DOMAIN = "zhalt"
PLATFORMS: list[str] = ["binary_sensor"]

DEFAULT_HOST = "172.217.28.1"
DEFAULT_PORT = 81
DEFAULT_NAME = "Zhalt Evolution Connect"

POLL_INTERVAL_S = 1.5
HANDSHAKE_TIMEOUT_S = 10.0
WS_RECV_TIMEOUT_S = 10.0
DAT_STALE_AFTER_S = 10.0  # binary_sensor.zhalt_connected goes off after this gap
RECONNECT_BACKOFF_S: tuple[float, ...] = (2, 4, 8, 16, 32, 60, 60, 60)
CLOCK_DRIFT_WARN_S = 300

STORAGE_VERSION = 1
STORAGE_KEY = f"{DOMAIN}_settings_cache"

CONF_HOST = "host"
CONF_PORT = "port"

# Service names
SERVICE_MIST = "mist"
SERVICE_STOP = "stop"
SERVICE_REFRESH = "refresh"

ATTR_DURATION = "duration"
