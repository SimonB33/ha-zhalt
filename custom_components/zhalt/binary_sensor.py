"""Binary sensors for the Zhalt integration."""
from __future__ import annotations

from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import protocol
from .const import DOMAIN
from .coordinator import ZhaltCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    coordinator: ZhaltCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        [
            ZhaltMistingBinarySensor(coordinator, entry),
            ZhaltConnectedBinarySensor(coordinator, entry),
        ]
    )


class _ZhaltBaseBinarySensor(CoordinatorEntity[ZhaltCoordinator], BinarySensorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._entry_id = entry.entry_id
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name="Zhalt Evolution Connect",
            manufacturer="Freezanz",
            model="Zhalt Evolution Connect",
            configuration_url=f"http://{coordinator.host}/Zhalt",
        )


class ZhaltMistingBinarySensor(_ZhaltBaseBinarySensor):
    _attr_translation_key = "misting"
    _attr_name = "Misting"
    _attr_device_class = BinarySensorDeviceClass.RUNNING

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_misting"

    @property
    def is_on(self) -> bool | None:
        d = self.coordinator.data
        if not d:
            return None
        return d.get("operating_mode") == protocol.OP_MISTING

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """v0.1.9 diagnostic: expose candidate G_dat token values so we can
        identify which actually tracks "pump running" under Manual mode.
        Polling the entity during a 70s mist gives a per-update snapshot
        without needing access to the home-assistant.log file.
        Remove once parse_g_dat exposes is_pump_running."""
        d = self.coordinator.data
        if not d:
            return None
        return {
            "diag_operating_mode": d.get("operating_mode"),
            "diag_substate": d.get("substate"),
            "diag_cycle_state": d.get("cycle_state"),
            "diag_elapsed_in_cycle": d.get("elapsed_in_cycle"),
            "diag_remaining_sec": d.get("remaining_sec"),
            "diag_mist_done": d.get("mist_done"),
            "diag_stop_done": d.get("stop_done"),
            "diag_active_cycle_id": d.get("active_cycle_id"),
        }


class ZhaltConnectedBinarySensor(_ZhaltBaseBinarySensor):
    _attr_translation_key = "connected"
    _attr_name = "Connected"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_connected"

    @property
    def is_on(self) -> bool:
        return self.coordinator.connected

    @property
    def available(self) -> bool:
        # Always show this entity even if the coordinator is offline; that's
        # the whole point of a connectivity sensor.
        return True
