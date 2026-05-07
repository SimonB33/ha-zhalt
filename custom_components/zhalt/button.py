"""Buttons for the Zhalt integration."""
from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

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
            ZhaltMistNowButton(coordinator, entry),
            ZhaltStopButton(coordinator, entry),
            ZhaltTestPumpButton(coordinator, entry),
            ZhaltTestLedButton(coordinator, entry),
            ZhaltTestBuzzerButton(coordinator, entry),
        ]
    )


class _ZhaltBaseButton(ButtonEntity):
    _attr_has_entity_name = True
    _action: str = ""

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        self._coordinator = coordinator
        self._entry_id = entry.entry_id
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.entry_id)},
            name="Zhalt Evolution Connect",
            manufacturer="Freezanz",
            model="Zhalt Evolution Connect",
            configuration_url=f"http://{coordinator.host}/Zhalt",
        )

    async def async_press(self) -> None:
        try:
            await self._coordinator.fire_action(self._action)
        except RuntimeError as err:
            raise HomeAssistantError(f"Zhalt device unreachable: {err}") from err


class ZhaltMistNowButton(_ZhaltBaseButton):
    _attr_translation_key = "mist_now"
    _attr_name = "Mist now"
    _attr_icon = "mdi:sprinkler-variant"
    _action = "mist_send"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_mist_now"


class ZhaltStopButton(_ZhaltBaseButton):
    _attr_translation_key = "stop"
    _attr_name = "Stop"
    _attr_icon = "mdi:stop-circle"
    _action = "stop_send"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_stop"


class ZhaltTestPumpButton(_ZhaltBaseButton):
    _attr_translation_key = "test_pump"
    _attr_name = "Test pump"
    _attr_icon = "mdi:pump"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _action = "provapump_send"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_test_pump"


class ZhaltTestLedButton(_ZhaltBaseButton):
    _attr_translation_key = "test_led"
    _attr_name = "Test LED"
    _attr_icon = "mdi:led-on"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _action = "provaled_send"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_test_led"


class ZhaltTestBuzzerButton(_ZhaltBaseButton):
    _attr_translation_key = "test_buzzer"
    _attr_name = "Test buzzer"
    _attr_icon = "mdi:bullhorn"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _action = "provabuz_send"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_test_buzzer"
