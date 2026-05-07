"""Config flow for Zhalt Evolution Connect."""
from __future__ import annotations

import asyncio
from typing import Any

import voluptuous as vol
import websockets
from homeassistant import config_entries
from homeassistant.data_entry_flow import FlowResult

from . import protocol
from .const import (
    CONF_HOST,
    CONF_PORT,
    DEFAULT_HOST,
    DEFAULT_NAME,
    DEFAULT_PORT,
    DOMAIN,
    HANDSHAKE_TIMEOUT_S,
)


DATA_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_HOST, default=DEFAULT_HOST): str,
        vol.Required(CONF_PORT, default=DEFAULT_PORT): int,
    }
)


async def _probe(host: str, port: int) -> None:
    """Open a WebSocket, expect 'CiaO' within timeout. Raise on failure."""
    url = f"ws://{host}:{port}"
    async with websockets.connect(url, open_timeout=HANDSHAKE_TIMEOUT_S) as ws:
        greeting = await asyncio.wait_for(ws.recv(), timeout=HANDSHAKE_TIMEOUT_S)
        text = greeting if isinstance(greeting, str) else greeting.decode()
        if text.strip() != protocol.GREETING:
            raise RuntimeError(f"unexpected greeting: {text!r}")


class ZhaltConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Single-step user flow asking for host/port and probing the device."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> FlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            host = user_input[CONF_HOST]
            port = user_input[CONF_PORT]
            await self.async_set_unique_id(f"{host}:{port}")
            self._abort_if_unique_id_configured()
            try:
                await _probe(host, port)
            except (asyncio.TimeoutError, OSError, websockets.WebSocketException):
                errors["base"] = "cannot_connect"
            except RuntimeError:
                errors["base"] = "bad_greeting"
            except Exception:  # noqa: BLE001
                errors["base"] = "unknown"
            else:
                return self.async_create_entry(title=DEFAULT_NAME, data=user_input)

        return self.async_show_form(
            step_id="user", data_schema=DATA_SCHEMA, errors=errors
        )
