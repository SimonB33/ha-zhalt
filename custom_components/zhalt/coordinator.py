"""WebSocket coordinator for the Zhalt Evolution Connect integration.

State machine (per spec section 4.2 / Phase 3):
  DISCONNECTED -> CONNECTING -> HANDSHAKING -> CONNECTED
  any failure  -> RECONNECTING (backoff 2,4,8,16,32,60,60,60s)

Two cooperative tasks once connected:
  - receive loop:   reads frames, dispatches G_imp/G_dat parses
  - keepalive loop: sends P_dat every 1.5s (carrying any pending action)

Pending actions (mist/stop/etc) are coalesced into the next P_dat to avoid
racing the keepalive task.
"""
from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timedelta
from enum import Enum
from typing import Any

import websockets
from homeassistant.core import HomeAssistant
from homeassistant.helpers.storage import Store
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator
from homeassistant.util import dt as dt_util

from . import protocol
from .const import (
    CLOCK_DRIFT_WARN_S,
    DAT_STALE_AFTER_S,
    DOMAIN,
    HANDSHAKE_TIMEOUT_S,
    POLL_INTERVAL_S,
    RECONNECT_BACKOFF_S,
    STORAGE_KEY,
    STORAGE_VERSION,
    WS_RECV_TIMEOUT_S,
)

_LOGGER = logging.getLogger(__name__)


class ConnState(str, Enum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    HANDSHAKING = "handshaking"
    CONNECTED = "connected"
    RECONNECTING = "reconnecting"


class ZhaltCoordinator(DataUpdateCoordinator[dict[str, Any] | None]):
    """Manages the WebSocket connection and exposes parsed state to entities."""

    def __init__(self, hass: HomeAssistant, *, host: str, port: int) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN}_{host}",
            update_interval=None,  # push-driven; no polling by HA
        )
        self.host = host
        self.port = port
        self.url = f"ws://{host}:{port}"

        self.conn_state: ConnState = ConnState.DISCONNECTED
        self.settings: dict[str, Any] | None = None
        self.last_dat_at: datetime | None = None

        self._ws: Any | None = None
        self._connection_task: asyncio.Task[None] | None = None
        self._recv_task: asyncio.Task[None] | None = None
        self._keepalive_task: asyncio.Task[None] | None = None
        self._stopped = asyncio.Event()
        self._pending_actions: dict[str, int] = {}
        self._auto_stop_task: asyncio.Task[None] | None = None
        self._cached_original_settings: dict[str, Any] | None = None
        self._store: Store = Store(hass, STORAGE_VERSION, STORAGE_KEY)
        self._got_initial_g_imp = asyncio.Event()

    # ---- public properties ---------------------------------------------------

    @property
    def connected(self) -> bool:
        if self.conn_state != ConnState.CONNECTED:
            return False
        if self.last_dat_at is None:
            return False
        return (dt_util.utcnow() - self.last_dat_at).total_seconds() < DAT_STALE_AFTER_S

    @property
    def is_misting(self) -> bool:
        d = self.data
        return bool(d and d.get("operating_mode") == protocol.OP_MISTING)

    # ---- lifecycle -----------------------------------------------------------

    async def async_start(self) -> None:
        """Load cached settings and kick off the connection task."""
        cached = await self._store.async_load()
        if isinstance(cached, dict) and cached.get("cycles"):
            self._cached_original_settings = cached
            _LOGGER.debug("loaded cached original settings from store")
        self._connection_task = self.hass.async_create_background_task(
            self._connection_loop(), name=f"{DOMAIN}_connection"
        )
        # Wait briefly for first G_imp so config_flow / setup can fail fast on
        # bad connection details, but don't block setup forever.
        try:
            await asyncio.wait_for(self._got_initial_g_imp.wait(), timeout=HANDSHAKE_TIMEOUT_S * 2)
        except asyncio.TimeoutError:
            _LOGGER.warning("no G_imp received within startup window; continuing in background")

    async def async_shutdown(self) -> None:
        self._stopped.set()
        for task in (self._auto_stop_task, self._keepalive_task, self._recv_task, self._connection_task):
            if task and not task.done():
                task.cancel()
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:  # noqa: BLE001
                pass
        self._ws = None

    # ---- public API used by entities / services -----------------------------

    async def fire_action(self, name: str) -> None:
        """Queue a single P_dat action flag for the next keepalive send."""
        if name not in protocol.P_DAT_ACTION_KEYS:
            raise ValueError(f"unknown action {name!r}")
        self._pending_actions[name] = 1

    async def fire_mist_with_duration(self, seconds: int) -> None:
        """Fire mist and schedule auto-stop after `seconds`. Idempotent if already misting."""
        if self.is_misting:
            _LOGGER.warning("mist requested while already misting; ignoring")
            return
        if self._auto_stop_task and not self._auto_stop_task.done():
            _LOGGER.warning("auto-stop timer already running; ignoring duplicate mist")
            return
        await self.fire_action("mist_send")
        self._auto_stop_task = self.hass.async_create_task(self._auto_stop(seconds))

    async def _auto_stop(self, seconds: int) -> None:
        try:
            await asyncio.sleep(seconds)
            await self.fire_action("stop_send")
        except asyncio.CancelledError:
            raise

    async def write_settings(self, new_settings: dict[str, Any]) -> None:
        """Send a B-form P_imp with the given settings; refreshes self.settings on echo."""
        if self._ws is None or self.conn_state != ConnState.CONNECTED:
            raise RuntimeError("not connected")
        frame = protocol.build_p_imp_settings(new_settings, datetime.now())
        await self._ws.send(frame)

    async def disable_all_cycles(self) -> None:
        if not self.settings:
            raise RuntimeError("no settings observed yet")
        await self.write_settings(protocol.disable_all_cycles(self.settings))

    async def restore_cycles(self) -> None:
        if self._cached_original_settings is None:
            raise RuntimeError("no cached original settings to restore")
        await self.write_settings(self._cached_original_settings)

    async def refresh_settings(self) -> None:
        """Re-handshake to force a fresh G_imp."""
        if self._ws is None:
            return
        await self._ws.send(protocol.build_p_imp_handshake(datetime.now()))

    # ---- connection loop -----------------------------------------------------

    async def _connection_loop(self) -> None:
        attempt = 0
        while not self._stopped.is_set():
            try:
                self.conn_state = ConnState.CONNECTING
                _LOGGER.debug("connecting to %s", self.url)
                async with websockets.connect(self.url, open_timeout=HANDSHAKE_TIMEOUT_S) as ws:
                    self._ws = ws
                    await self._handshake(ws)
                    self.conn_state = ConnState.CONNECTED
                    attempt = 0
                    self._recv_task = asyncio.create_task(self._recv_loop(ws))
                    self._keepalive_task = asyncio.create_task(self._keepalive_loop(ws))
                    done, pending = await asyncio.wait(
                        {self._recv_task, self._keepalive_task},
                        return_when=asyncio.FIRST_COMPLETED,
                    )
                    for t in pending:
                        t.cancel()
                    for t in done:
                        exc = t.exception()
                        if exc:
                            raise exc
            except asyncio.CancelledError:
                raise
            except (OSError, websockets.WebSocketException, asyncio.TimeoutError) as e:
                _LOGGER.warning("connection error: %s", e)
            except Exception:  # noqa: BLE001
                _LOGGER.exception("unexpected error in connection loop")
            finally:
                self._ws = None
                if self.conn_state != ConnState.DISCONNECTED:
                    self.conn_state = ConnState.RECONNECTING
                # Wake any sensor that's checking connected state.
                self.async_set_updated_data(self.data)

            if self._stopped.is_set():
                break
            delay = RECONNECT_BACKOFF_S[min(attempt, len(RECONNECT_BACKOFF_S) - 1)]
            attempt += 1
            try:
                await asyncio.wait_for(self._stopped.wait(), timeout=delay)
            except asyncio.TimeoutError:
                pass

    async def _handshake(self, ws: Any) -> None:
        self.conn_state = ConnState.HANDSHAKING
        greeting = await asyncio.wait_for(ws.recv(), timeout=HANDSHAKE_TIMEOUT_S)
        greeting_text = greeting if isinstance(greeting, str) else greeting.decode()
        if greeting_text.strip() != protocol.GREETING:
            raise RuntimeError(f"expected CiaO, got {greeting_text!r}")
        await ws.send(protocol.build_p_imp_handshake(datetime.now()))

    async def _recv_loop(self, ws: Any) -> None:
        async for raw in ws:
            text = raw if isinstance(raw, str) else raw.decode("utf-8", errors="replace")
            if text.startswith("G_dat"):
                self._handle_g_dat(text)
            elif text.startswith("G_imp"):
                self._handle_g_imp(text)
            else:
                _LOGGER.debug("unknown frame: %s", text[:80])

    async def _keepalive_loop(self, ws: Any) -> None:
        while True:
            actions = self._pending_actions
            self._pending_actions = {}
            await ws.send(protocol.build_p_dat(actions))
            await asyncio.sleep(POLL_INTERVAL_S)

    # ---- frame handlers ------------------------------------------------------

    def _handle_g_imp(self, text: str) -> None:
        try:
            parsed = protocol.parse_g_imp(text)
        except Exception:  # noqa: BLE001
            _LOGGER.warning("failed to parse G_imp: %.120s", text)
            return
        self.settings = parsed
        self._got_initial_g_imp.set()
        # Cache the first observed settings that have at least one cycle enabled
        # so the master switch can restore them later.
        if self._cached_original_settings is None and any(
            c.get("act") for c in parsed["cycles"].values()
        ):
            self._cached_original_settings = parsed
            self.hass.async_create_task(self._store.async_save(parsed))
            _LOGGER.info("cached original Zhalt settings for restore")

    def _handle_g_dat(self, text: str) -> None:
        try:
            parsed = protocol.parse_g_dat(text)
        except Exception:  # noqa: BLE001
            _LOGGER.warning("failed to parse G_dat: %.120s", text)
            return
        self.last_dat_at = dt_util.utcnow()
        self._maybe_warn_clock_drift(parsed)
        self.async_set_updated_data(parsed)

    def _maybe_warn_clock_drift(self, parsed: dict[str, Any]) -> None:
        d = parsed
        try:
            device_clock = datetime(
                d["device_year"], d["device_month"], d["device_day"],
                d["device_hour"], d["device_minute"], d["device_second"],
            )
        except (KeyError, TypeError, ValueError):
            return
        ha_now = dt_util.now().replace(tzinfo=None)
        drift = abs((ha_now - device_clock).total_seconds())
        if drift > CLOCK_DRIFT_WARN_S and not getattr(self, "_warned_drift", False):
            _LOGGER.warning(
                "Zhalt clock drift: device=%s, HA=%s, drift=%.0fs",
                device_clock, ha_now, drift,
            )
            self._warned_drift = True
