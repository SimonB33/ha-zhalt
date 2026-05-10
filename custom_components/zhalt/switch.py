"""Switch for the Zhalt integration."""
from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
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
    async_add_entities([ZhaltOnboardSchedulerSwitch(coordinator, entry)])


class ZhaltOnboardSchedulerSwitch(CoordinatorEntity[ZhaltCoordinator], SwitchEntity):
    _attr_has_entity_name = True
    _attr_translation_key = "onboard_scheduler"
    _attr_name = "Onboard scheduler"
    _attr_icon = "mdi:calendar-clock"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.entry_id}_onboard_scheduler"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name="Zhalt Evolution Connect",
            manufacturer="Freezanz",
            model="Zhalt Evolution Connect",
            configuration_url=f"http://{coordinator.host}/Zhalt",
        )

    @property
    def is_on(self) -> bool | None:
        s = self.coordinator.settings
        if not s:
            return None
        cycles = s.get("cycles") or {}
        return any(c.get("act") == 1 for c in cycles.values())

    async def async_turn_on(self, **kwargs: Any) -> None:
        try:
            await self.coordinator.restore_cycles()
        except RuntimeError as err:
            raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err

    async def async_turn_off(self, **kwargs: Any) -> None:
        try:
            await self.coordinator.disable_all_cycles()
        except RuntimeError as err:
            raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err
