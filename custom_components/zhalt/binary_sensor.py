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
    def available(self) -> bool:
        return self.coordinator.connected

    @property
    def is_on(self) -> bool | None:
        d = self.coordinator.data
        if not d:
            return None
        return d.get("operating_mode") == protocol.OP_MISTING


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
