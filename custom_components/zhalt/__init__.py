"""Zhalt Evolution Connect integration for Home Assistant."""
from __future__ import annotations

import logging

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv

from .const import (
    ATTR_DURATION,
    CONF_HOST,
    CONF_PORT,
    DOMAIN,
    PLATFORMS,
    SERVICE_MIST,
    SERVICE_REFRESH,
    SERVICE_STOP,
)
from .coordinator import ZhaltCoordinator

_LOGGER = logging.getLogger(__name__)

MIST_SCHEMA = vol.Schema(
    {vol.Required(ATTR_DURATION): vol.All(vol.Coerce(int), vol.Range(min=1, max=120))}
)
EMPTY_SCHEMA = vol.Schema({})


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up Zhalt from a config entry."""
    coordinator = ZhaltCoordinator(
        hass,
        entry_id=entry.entry_id,
        host=entry.data[CONF_HOST],
        port=entry.data[CONF_PORT],
    )
    await coordinator.async_start()

    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator
    if PLATFORMS:
        await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    _async_register_services(hass)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    if PLATFORMS:
        unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
        if not unloaded:
            return False
    coordinator: ZhaltCoordinator | None = hass.data.get(DOMAIN, {}).pop(entry.entry_id, None)
    if coordinator:
        await coordinator.async_shutdown()

    if not hass.data.get(DOMAIN):
        for svc in (SERVICE_MIST, SERVICE_STOP, SERVICE_REFRESH):
            if hass.services.has_service(DOMAIN, svc):
                hass.services.async_remove(DOMAIN, svc)
    return True


def _all_coordinators(hass: HomeAssistant) -> list[ZhaltCoordinator]:
    return list(hass.data.get(DOMAIN, {}).values())


def _async_register_services(hass: HomeAssistant) -> None:
    if hass.services.has_service(DOMAIN, SERVICE_MIST):
        return

    async def _handle_mist(call: ServiceCall) -> None:
        duration = int(call.data[ATTR_DURATION])
        for coordinator in _all_coordinators(hass):
            try:
                await coordinator.fire_mist_with_duration(duration)
            except RuntimeError as err:
                raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    async def _handle_stop(call: ServiceCall) -> None:
        for coordinator in _all_coordinators(hass):
            try:
                await coordinator.fire_action("stop_send")
            except RuntimeError as err:
                raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    async def _handle_refresh(call: ServiceCall) -> None:
        for coordinator in _all_coordinators(hass):
            try:
                await coordinator.refresh_settings()
            except RuntimeError as err:
                raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    hass.services.async_register(DOMAIN, SERVICE_MIST, _handle_mist, schema=MIST_SCHEMA)
    hass.services.async_register(DOMAIN, SERVICE_STOP, _handle_stop, schema=EMPTY_SCHEMA)
    hass.services.async_register(
        DOMAIN, SERVICE_REFRESH, _handle_refresh, schema=EMPTY_SCHEMA
    )
