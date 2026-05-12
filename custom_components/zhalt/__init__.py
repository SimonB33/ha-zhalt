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
    SERVICE_DISABLE_CYCLES,
    SERVICE_LOG_CYCLES,
    SERVICE_MIST,
    SERVICE_REFRESH,
    SERVICE_RESTORE_CYCLES,
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
        for svc in (
            SERVICE_MIST,
            SERVICE_STOP,
            SERVICE_REFRESH,
            SERVICE_DISABLE_CYCLES,
            SERVICE_RESTORE_CYCLES,
            SERVICE_LOG_CYCLES,
        ):
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

    async def _handle_disable_cycles(call: ServiceCall) -> None:
        for coordinator in _all_coordinators(hass):
            try:
                await coordinator.disable_all_cycles()
            except RuntimeError as err:
                raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    async def _handle_restore_cycles(call: ServiceCall) -> None:
        for coordinator in _all_coordinators(hass):
            try:
                await coordinator.restore_cycles()
            except RuntimeError as err:
                raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    async def _handle_log_cycles(call: ServiceCall) -> None:
        for coordinator in _all_coordinators(hass):
            cached = coordinator._cached_original_settings
            current = coordinator.settings
            _LOGGER.warning(
                "log_cycles: cached_original_settings present=%s, current_settings present=%s",
                cached is not None,
                current is not None,
            )
            for label, src in (("cached", cached), ("current", current)):
                if src is None or "cycles" not in src:
                    _LOGGER.warning("log_cycles[%s]: no cycles data", label)
                    continue
                for cycle_label, c in src["cycles"].items():
                    _LOGGER.warning(
                        "log_cycles[%s] %s: act=%s mode=%s start=%02d:%02d days_bm=%s "
                        "dur_s=%s end=%02d:%02d work_s=%s pause_m=%s",
                        label,
                        cycle_label,
                        c.get("act"),
                        c.get("mode"),
                        c.get("start_hour", 0) or 0,
                        c.get("start_minute", 0) or 0,
                        c.get("days_bitmap"),
                        c.get("duration_seconds"),
                        c.get("end_hour", 0) or 0,
                        c.get("end_minute", 0) or 0,
                        c.get("work_seconds"),
                        c.get("pause_minutes"),
                    )

    hass.services.async_register(DOMAIN, SERVICE_MIST, _handle_mist, schema=MIST_SCHEMA)
    hass.services.async_register(DOMAIN, SERVICE_STOP, _handle_stop, schema=EMPTY_SCHEMA)
    hass.services.async_register(
        DOMAIN, SERVICE_REFRESH, _handle_refresh, schema=EMPTY_SCHEMA
    )
    hass.services.async_register(
        DOMAIN, SERVICE_DISABLE_CYCLES, _handle_disable_cycles, schema=EMPTY_SCHEMA
    )
    hass.services.async_register(
        DOMAIN, SERVICE_RESTORE_CYCLES, _handle_restore_cycles, schema=EMPTY_SCHEMA
    )
    hass.services.async_register(
        DOMAIN, SERVICE_LOG_CYCLES, _handle_log_cycles, schema=EMPTY_SCHEMA
    )
