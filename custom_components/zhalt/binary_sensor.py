"""Binary sensors for the Zhalt integration."""
from __future__ import annotations

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

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
        # The device's operating_mode flips Stopped (6) at the phase 1 → phase 2
        # transition (~70-80% through a cycle) while the pump keeps delivering
        # visible spray through phase 2. Use cycle_state instead, which stays
        # in {1, 2} for the full active-spray window. See protocol.py for the
        # state-machine notes.
        return bool(d.get("is_pump_running"))


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
