"""Sensors for the Zhalt integration."""
from __future__ import annotations

from datetime import datetime
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity
from homeassistant.util import dt as dt_util

from . import protocol
from .const import DOMAIN
from .coordinator import ZhaltCoordinator

OPERATING_MODE_OPTIONS = ["Stopped", "Standby", "Misting"]
ACTIVE_CYCLE_OPTIONS = [
    "None",
    "Manual",
    "Cycle1",
    "Cycle2",
    "Cycle3",
    "Cycle4",
    "Cycle5",
    "Cycle6",
    "Cycle7",
    "Cycle8",
    "Cycle9",
]
MACHINE_TYPE_OPTIONS = ["Evolution", "Portable"]


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    coordinator: ZhaltCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        [
            ZhaltStateSensor(coordinator, entry),
            ZhaltActiveCycleSensor(coordinator, entry),
            ZhaltElapsedSecondsSensor(coordinator, entry),
            ZhaltPlannedDurationSensor(coordinator, entry),
            ZhaltRemainingSecondsSensor(coordinator, entry),
            ZhaltFirmwareVersionSensor(coordinator, entry),
            ZhaltDeviceClockSensor(coordinator, entry),
            ZhaltMachineTypeSensor(coordinator, entry),
            ZhaltTickSensor(coordinator, entry),
        ]
    )


class _ZhaltBaseSensor(CoordinatorEntity[ZhaltCoordinator], SensorEntity):
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

    @property
    def available(self) -> bool:
        return self.coordinator.connected


class ZhaltStateSensor(_ZhaltBaseSensor):
    _attr_translation_key = "state"
    _attr_name = "State"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = OPERATING_MODE_OPTIONS

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_state"

    @property
    def native_value(self) -> str | None:
        d = self.coordinator.data
        if not d:
            return None
        name = d.get("operating_mode_name")
        return name if name in OPERATING_MODE_OPTIONS else None


class ZhaltActiveCycleSensor(_ZhaltBaseSensor):
    _attr_translation_key = "active_cycle"
    _attr_name = "Active cycle"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = ACTIVE_CYCLE_OPTIONS

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_active_cycle"

    @property
    def native_value(self) -> str | None:
        d = self.coordinator.data
        if not d:
            return None
        name = d.get("active_cycle_name")
        return name if name in ACTIVE_CYCLE_OPTIONS else None


class _DurationSensor(_ZhaltBaseSensor):
    _attr_device_class = SensorDeviceClass.DURATION
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS
    _attr_state_class = SensorStateClass.MEASUREMENT


class ZhaltElapsedSecondsSensor(_DurationSensor):
    _attr_translation_key = "elapsed_seconds"
    _attr_name = "Elapsed seconds"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_elapsed_seconds"

    @property
    def native_value(self) -> int | None:
        d = self.coordinator.data
        return d.get("elapsed_in_cycle") if d else None


class ZhaltPlannedDurationSensor(_DurationSensor):
    _attr_translation_key = "planned_duration"
    _attr_name = "Planned duration"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_planned_duration"

    @property
    def native_value(self) -> int | None:
        d = self.coordinator.data
        return d.get("planned_duration_sec") if d else None


class ZhaltRemainingSecondsSensor(_DurationSensor):
    _attr_translation_key = "remaining_seconds"
    _attr_name = "Remaining seconds"

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_remaining_seconds"

    @property
    def native_value(self) -> int | None:
        d = self.coordinator.data
        return d.get("remaining_sec") if d else None


class ZhaltFirmwareVersionSensor(_ZhaltBaseSensor):
    _attr_translation_key = "firmware_version"
    _attr_name = "Firmware version"
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_firmware_version"

    @property
    def native_value(self) -> str | None:
        s = self.coordinator.settings
        if not s:
            return None
        minor = s.get("firmware_minor")
        return f"2.{minor}" if minor else None


class ZhaltDeviceClockSensor(_ZhaltBaseSensor):
    _attr_translation_key = "device_clock"
    _attr_name = "Device clock"
    _attr_device_class = SensorDeviceClass.TIMESTAMP
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_device_clock"

    @property
    def native_value(self) -> datetime | None:
        d = self.coordinator.data
        if not d:
            return None
        try:
            year = d.get("device_year")
            month = d.get("device_month")
            day = d.get("device_day")
            hour = d.get("device_hour")
            minute = d.get("device_minute")
            second = d.get("device_second")
            if None in (year, month, day, hour, minute, second):
                return None
            # Device firmware emits a 2-digit year (e.g. 26 → 2026).
            if year < 100:
                year += 2000
            naive = datetime(year, month, day, hour, minute, second)
            return naive.replace(tzinfo=dt_util.DEFAULT_TIME_ZONE)
        except (TypeError, ValueError):
            return None


class ZhaltMachineTypeSensor(_ZhaltBaseSensor):
    _attr_translation_key = "machine_type"
    _attr_name = "Machine type"
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = MACHINE_TYPE_OPTIONS
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_machine_type"

    @property
    def native_value(self) -> str | None:
        s = self.coordinator.settings
        if not s:
            return None
        mt = s.get("machine_type")
        if mt == 1:
            return "Evolution"
        if mt == 0:
            return "Portable"
        return None


class ZhaltTickSensor(_ZhaltBaseSensor):
    _attr_translation_key = "tick"
    _attr_name = "Tick"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_entity_registry_enabled_default = False
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator: ZhaltCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry)
        self._attr_unique_id = f"{entry.entry_id}_tick"

    @property
    def native_value(self) -> int | None:
        d = self.coordinator.data
        return d.get("tick") if d else None
