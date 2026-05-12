"""WebSocket coordinator for the Zhalt Evolution Connect integration.

On-demand session model:
- The coordinator stays disconnected by default to spare the device.
- An action (mist, stop, settings change, refresh) opens a session: handshake +
  P_dat keepalive at 1.5s. Each action sets/extends an "active until" deadline.
- When the deadline passes the session closes cleanly.
- Sensors keep showing the last observed value when no session is active;
  binary_sensor.zhalt_connected is the freshness indicator.

State machine inside a session:
  DISCONNECTED -> CONNECTING -> HANDSHAKING -> CONNECTED -> (timer or error)
                                                          -> DISCONNECTED
"""
from __future__ import annotations

import asyncio
import logging
import time
from datetime import datetime
from enum import Enum
from typing import Any

import websockets
from homeassistant.core import HomeAssistant
from homeassistant.helpers.storage import Store
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator
from homeassistant.util import dt as dt_util

from . import protocol
from .const import (
    ACTION_HOLD_DEFAULT_S,
    ACTION_HOLD_S,
    CLOCK_DRIFT_WARN_S,
    DAT_STALE_AFTER_S,
    DOMAIN,
    HANDSHAKE_TIMEOUT_S,
    HEALTHCHECK_HOLD_S,
    HEALTHCHECK_INTERVAL_S,
    HEALTHCHECK_WINDOW_HOURS,
    MAX_CONSECUTIVE_FAILURES,
    POLL_INTERVAL_S,
    RECONNECT_BACKOFF_S,
    REFRESH_HOLD_S,
    RELOAD_COOLDOWN_S,
    SESSION_ESTABLISH_RETRY_BACKOFF_S,
    SETTINGS_HOLD_S,
    STARTUP_HOLD_S,
    STOP_RETRY_BACKOFF_S,
    STOP_RETRY_TOTAL_S,
    STORAGE_KEY,
    STORAGE_VERSION,
)

_LOGGER = logging.getLogger(__name__)


class ConnState(str, Enum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    HANDSHAKING = "handshaking"
    CONNECTED = "connected"


class ZhaltCoordinator(DataUpdateCoordinator[dict[str, Any] | None]):
    """Manages on-demand WebSocket sessions and exposes parsed state to entities."""

    def __init__(
        self, hass: HomeAssistant, *, entry_id: str, host: str, port: int
    ) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN}_{host}",
            update_interval=None,  # push-driven; no polling by HA
        )
        self._entry_id = entry_id
        self.host = host
        self.port = port
        self.url = f"ws://{host}:{port}"

        self.conn_state: ConnState = ConnState.DISCONNECTED
        self.settings: dict[str, Any] | None = None
        self.last_dat_at: datetime | None = None

        self._ws: Any | None = None
        self._session_task: asyncio.Task[None] | None = None
        self._auto_stop_task: asyncio.Task[None] | None = None
        self._healthcheck_task: asyncio.Task[None] | None = None
        self._stopped = asyncio.Event()
        self._pending_actions: dict[str, int] = {}
        self._cached_original_settings: dict[str, Any] | None = None
        self._store: Store = Store(hass, STORAGE_VERSION, STORAGE_KEY)
        self._got_initial_g_imp = asyncio.Event()
        self._connected_event = asyncio.Event()
        self._active_until_mono: float = 0.0
        # Self-heal counters: track session_loop invocations that fail to reach
        # CONNECTED. After MAX_CONSECUTIVE_FAILURES in a row, schedule a reload.
        self._consecutive_failures: int = 0
        self._last_reload_mono: float = 0.0

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
        """Load cached settings and run a brief startup session to fetch G_imp."""
        cached = await self._store.async_load()
        if isinstance(cached, dict) and cached.get("cycles"):
            self._cached_original_settings = cached
            _LOGGER.debug("loaded cached original settings from store")
        # Best-effort startup session; don't block setup if device is offline.
        try:
            await self._ensure_session(STARTUP_HOLD_S, wait_for_g_imp=True)
        except Exception as err:  # noqa: BLE001
            _LOGGER.warning("startup session failed: %s (will retry on first action)", err)
        # Background periodic health check during typical-use hours.
        self._healthcheck_task = self.hass.async_create_background_task(
            self._healthcheck_loop(), name=f"{DOMAIN}_healthcheck"
        )

    async def async_shutdown(self) -> None:
        self._stopped.set()
        for task in (self._auto_stop_task, self._healthcheck_task, self._session_task):
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
        """Open a session if needed and queue a P_dat action flag."""
        if name not in protocol.P_DAT_ACTION_KEYS:
            raise ValueError(f"unknown action {name!r}")
        hold = ACTION_HOLD_S.get(name, ACTION_HOLD_DEFAULT_S)
        await self._ensure_session(hold)
        self._pending_actions[name] = 1

    async def _ensure_session_with_retry(
        self, hold_seconds: float, *, wait_for_g_imp: bool = False
    ) -> None:
        """Like _ensure_session but retries transient establish failures.

        A single "session failed to establish" can be a brief Wi-Fi flap or
        a DHCP renewal — retrying a handful of times with short backoff
        recovers from those without surfacing a hard error to the user.
        Each underlying attempt itself can take up to HANDSHAKE_TIMEOUT_S*2.
        """
        last_err: Exception | None = None
        attempts = len(SESSION_ESTABLISH_RETRY_BACKOFF_S) + 1
        for attempt, backoff in enumerate((0.0,) + SESSION_ESTABLISH_RETRY_BACKOFF_S):
            if backoff:
                try:
                    await asyncio.sleep(backoff)
                except asyncio.CancelledError:
                    raise
            try:
                await self._ensure_session(
                    hold_seconds, wait_for_g_imp=wait_for_g_imp
                )
                if attempt > 0:
                    _LOGGER.info(
                        "Zhalt session established on attempt %d/%d",
                        attempt + 1,
                        attempts,
                    )
                return
            except asyncio.CancelledError:
                raise
            except RuntimeError as err:
                last_err = err
                _LOGGER.warning(
                    "session establish attempt %d/%d failed: %s",
                    attempt + 1,
                    attempts,
                    err,
                )
        assert last_err is not None
        raise last_err

    async def fire_mist_with_duration(self, seconds: int) -> None:
        """Fire mist and schedule auto-stop after `seconds`. Idempotent if already misting."""
        if self.is_misting:
            _LOGGER.warning("mist requested while already misting; ignoring")
            return
        if self._auto_stop_task and not self._auto_stop_task.done():
            _LOGGER.warning("auto-stop timer already running; ignoring duplicate mist")
            return
        # Hold the session through mist + the full stop_send retry window so
        # the auto-stop doesn't have to cold-reconnect on a flaky network.
        # Retry the initial establish on transient failures so brief flaps
        # don't surface to the user as a hard "device unreachable" error.
        await self._ensure_session_with_retry(seconds + STOP_RETRY_TOTAL_S + 5.0)
        self._pending_actions["mist_send"] = 1
        self._auto_stop_task = self.hass.async_create_task(self._auto_stop(seconds))

    async def _auto_stop(self, seconds: int) -> None:
        """Sleep then send stop_send, retrying aggressively on failure.

        Stop is safety-critical: if it never lands, the device sprays in
        Manual mode indefinitely (no onboard timeout for Manual). Each retry
        re-opens the session via fire_action -> _ensure_session.
        """
        try:
            await asyncio.sleep(seconds)
        except asyncio.CancelledError:
            raise
        last_err: Exception | None = None
        for attempt, delay in enumerate(STOP_RETRY_BACKOFF_S):
            if delay:
                try:
                    await asyncio.sleep(delay)
                except asyncio.CancelledError:
                    raise
            try:
                await self.fire_action("stop_send")
                if attempt > 0:
                    _LOGGER.warning(
                        "stop_send succeeded on attempt %d/%d",
                        attempt + 1,
                        len(STOP_RETRY_BACKOFF_S),
                    )
                return
            except asyncio.CancelledError:
                raise
            except Exception as err:  # noqa: BLE001
                last_err = err
                _LOGGER.warning(
                    "stop_send attempt %d/%d failed: %s",
                    attempt + 1,
                    len(STOP_RETRY_BACKOFF_S),
                    err,
                )
        _LOGGER.error(
            "stop_send FAILED after %d attempts (~%ds total). Device may still "
            "be misting in Manual mode — manual intervention may be required. "
            "Last error: %s",
            len(STOP_RETRY_BACKOFF_S),
            STOP_RETRY_TOTAL_S,
            last_err,
        )
        # Fire a custom event so HA automations can alert without depending
        # on the `system_log: fire_event: true` config (off by default).
        self.hass.bus.async_fire(
            f"{DOMAIN}_runaway_spray",
            {
                "attempts": len(STOP_RETRY_BACKOFF_S),
                "total_seconds": STOP_RETRY_TOTAL_S,
                "last_error": str(last_err) if last_err else "",
                "conn_state": str(self.conn_state),
                "host": self.host,
            },
        )

    async def write_settings(self, new_settings: dict[str, Any]) -> None:
        """Send a B-form P_imp with the given settings; refreshes self.settings on echo."""
        await self._ensure_session(SETTINGS_HOLD_S)
        if self._ws is None or self.conn_state != ConnState.CONNECTED:
            raise RuntimeError("not connected")
        frame = protocol.build_p_imp_settings(new_settings, datetime.now())
        await self._ws.send(frame)

    async def disable_all_cycles(self) -> None:
        if not self.settings:
            await self._ensure_session(STARTUP_HOLD_S, wait_for_g_imp=True)
        if not self.settings:
            raise RuntimeError("no settings observed yet")
        await self.write_settings(protocol.disable_all_cycles(self.settings))

    async def restore_cycles(self) -> None:
        if self._cached_original_settings is None:
            raise RuntimeError("no cached original settings to restore")
        await self.write_settings(self._cached_original_settings)

    async def refresh_settings(self) -> None:
        """Open a session and re-handshake to force a fresh G_imp."""
        await self._ensure_session(REFRESH_HOLD_S)
        if self._ws is None:
            raise RuntimeError("not connected")
        await self._ws.send(protocol.build_p_imp_handshake(datetime.now()))

    # ---- session management -------------------------------------------------

    async def _ensure_session(
        self, hold_seconds: float, *, wait_for_g_imp: bool = False
    ) -> None:
        """Open a session if needed, extending the active deadline."""
        deadline = time.monotonic() + hold_seconds
        if deadline > self._active_until_mono:
            self._active_until_mono = deadline

        if self._session_task is None or self._session_task.done():
            self._connected_event.clear()
            if wait_for_g_imp:
                self._got_initial_g_imp.clear()
            self._session_task = self.hass.async_create_background_task(
                self._session_loop(), name=f"{DOMAIN}_session"
            )

        try:
            await asyncio.wait_for(
                self._connected_event.wait(), timeout=HANDSHAKE_TIMEOUT_S * 2
            )
        except asyncio.TimeoutError as err:
            raise RuntimeError("session failed to establish") from err

        if wait_for_g_imp:
            try:
                await asyncio.wait_for(
                    self._got_initial_g_imp.wait(), timeout=HANDSHAKE_TIMEOUT_S
                )
            except asyncio.TimeoutError:
                _LOGGER.warning(
                    "no G_imp received within %.0fs of session start", HANDSHAKE_TIMEOUT_S
                )

    async def _session_loop(self) -> None:
        """One on-demand session: connect, handshake, run until deadline expires."""
        attempt = 0
        had_connected = False
        try:
            while not self._stopped.is_set():
                try:
                    self.conn_state = ConnState.CONNECTING
                    _LOGGER.debug("opening session to %s", self.url)
                    async with websockets.connect(
                        self.url, open_timeout=HANDSHAKE_TIMEOUT_S
                    ) as ws:
                        self._ws = ws
                        await self._handshake(ws)
                        self.conn_state = ConnState.CONNECTED
                        attempt = 0
                        if not had_connected:
                            _LOGGER.info("Zhalt session established")
                        had_connected = True
                        self._consecutive_failures = 0
                        self._connected_event.set()
                        recv_task = asyncio.create_task(self._recv_loop(ws))
                        ka_task = asyncio.create_task(self._keepalive_loop(ws))
                        timer_task = asyncio.create_task(self._session_timer())
                        done, pending = await asyncio.wait(
                            {recv_task, ka_task, timer_task},
                            return_when=asyncio.FIRST_COMPLETED,
                        )
                        for t in pending:
                            t.cancel()
                        if timer_task in done:
                            _LOGGER.debug("session deadline reached; closing cleanly")
                            return
                        for t in done:
                            exc = t.exception()
                            if exc:
                                raise exc
                except asyncio.CancelledError:
                    raise
                except (OSError, websockets.WebSocketException, asyncio.TimeoutError) as e:
                    _LOGGER.warning("session error: %s", e)
                except Exception:  # noqa: BLE001
                    _LOGGER.exception("unexpected error in session loop")
                finally:
                    self._ws = None
                    self.conn_state = ConnState.DISCONNECTED
                    self.async_set_updated_data(self.data)

                if self._stopped.is_set():
                    return
                if time.monotonic() >= self._active_until_mono:
                    return
                delay = RECONNECT_BACKOFF_S[min(attempt, len(RECONNECT_BACKOFF_S) - 1)]
                attempt += 1
                try:
                    await asyncio.wait_for(self._stopped.wait(), timeout=delay)
                except asyncio.TimeoutError:
                    pass
        finally:
            self.conn_state = ConnState.DISCONNECTED
            self._connected_event.clear()
            self.async_set_updated_data(self.data)
            if not had_connected and not self._stopped.is_set():
                self._consecutive_failures += 1
                _LOGGER.info(
                    "Zhalt session attempt ended without connection (%d/%d consecutive)",
                    self._consecutive_failures,
                    MAX_CONSECUTIVE_FAILURES,
                )
                if self._consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                    self._maybe_request_self_reload()

    async def _healthcheck_loop(self) -> None:
        """Brief refresh session every HEALTHCHECK_INTERVAL_S during the active window."""
        start_h, end_h = HEALTHCHECK_WINDOW_HOURS
        while not self._stopped.is_set():
            try:
                await asyncio.wait_for(
                    self._stopped.wait(), timeout=HEALTHCHECK_INTERVAL_S
                )
                return
            except asyncio.TimeoutError:
                pass
            now_local = dt_util.now()
            if not (start_h <= now_local.hour < end_h):
                continue
            _LOGGER.info("Zhalt healthcheck firing")
            try:
                await self._ensure_session(HEALTHCHECK_HOLD_S)
            except Exception as err:  # noqa: BLE001
                _LOGGER.info("Zhalt healthcheck session failed: %s", err)

    def _maybe_request_self_reload(self) -> None:
        """Schedule an integration reload after persistent connection failure.

        Called from `_session_loop` when `_consecutive_failures` crosses the
        threshold. Rate-limited by `RELOAD_COOLDOWN_S` so we don't spin if the
        device is genuinely unreachable for an extended period.
        """
        now = time.monotonic()
        if now - self._last_reload_mono < RELOAD_COOLDOWN_S:
            _LOGGER.info(
                "Zhalt: reload threshold reached but within cooldown; skipping"
            )
            return
        self._last_reload_mono = now
        _LOGGER.warning(
            "Zhalt: %d consecutive failed sessions, triggering integration reload",
            self._consecutive_failures,
        )
        self.hass.async_create_task(
            self.hass.config_entries.async_reload(self._entry_id)
        )

    async def _session_timer(self) -> None:
        """Resolves when the active deadline passes, allowing the session to close."""
        while True:
            now = time.monotonic()
            remaining = self._active_until_mono - now
            if remaining <= 0:
                return
            await asyncio.sleep(remaining)

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
