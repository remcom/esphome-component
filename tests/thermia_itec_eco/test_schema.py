"""Schema validation tests — run without hardware, requires esphome installed."""

from collections.abc import Callable, Iterator
import contextlib
import importlib
from types import ModuleType

import pytest

import esphome.config_validation as cv
from esphome.core import ID
from esphome.types import ConfigType


@pytest.fixture
def thermia(load_component: Callable[[str], ModuleType]) -> ModuleType:
    return load_component("thermia_itec_eco")


@pytest.fixture
def schema(thermia: ModuleType) -> Callable[[ConfigType], ConfigType]:
    return thermia.CONFIG_SCHEMA


def test_minimal_config_accepted(schema: Callable[[ConfigType], ConfigType]) -> None:
    result = schema({})
    assert result["bus_timeout"].total_milliseconds == 30000
    assert "room_sensor" not in result


def test_bus_timeout_zero_disables(schema: Callable[[ConfigType], ConfigType]) -> None:
    assert schema({"bus_timeout": "0s"})["bus_timeout"].total_milliseconds == 0


def test_room_sensor_defaults(schema: Callable[[ConfigType], ConfigType]) -> None:
    room = schema({"room_sensor": {"temperature": "room_temp"}})["room_sensor"]
    assert room["timeout"].total_milliseconds == 3600000


def test_room_sensor_needs_temperature(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    with pytest.raises(cv.Invalid, match="temperature"):
        schema({"room_sensor": {}})


def test_room_sensor_timeout_zero_disables(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    room = schema({"room_sensor": {"temperature": "room_temp", "timeout": "0s"}})
    assert room["room_sensor"]["timeout"].total_milliseconds == 0


def test_platform_keys_map_to_distinct_enums(thermia: ModuleType) -> None:
    sensor_platform = importlib.import_module("components.thermia_itec_eco.sensor")
    binary_platform = importlib.import_module(
        "components.thermia_itec_eco.binary_sensor"
    )
    for table in (sensor_platform.SENSORS, binary_platform.BINARY_SENSORS):
        enums = [enum for enum, _ in table.values()]
        assert len(set(enums)) == len(enums)


def test_uart_requirements_follow_room_sensor(thermia: ModuleType) -> None:
    """TX is required only with room sensor emulation."""
    calls: list[dict[str, object]] = []

    def fake_final_validate(name: str, **kwargs: object) -> Callable[[ConfigType], None]:
        calls.append(kwargs)
        return lambda config: None

    original = thermia.uart.final_validate_device_schema
    thermia.uart.final_validate_device_schema = fake_final_validate
    try:
        thermia.FINAL_VALIDATE_SCHEMA({})
        thermia.FINAL_VALIDATE_SCHEMA({"room_sensor": {}})
    finally:
        thermia.uart.final_validate_device_schema = original
    assert [call["require_tx"] for call in calls] == [False, True]
    for call in calls:
        assert call["baud_rate"] == 9600
        assert call["parity"] == "EVEN"
        assert call["require_rx"] is True


class FakeFullConfig(dict):
    def __init__(self, hub_config: ConfigType) -> None:
        super().__init__(thermia_itec_eco=[hub_config])

    def get_path_for_id(self, _id: ID) -> list[object]:
        return ["thermia_itec_eco", 0, "id"]

    def get_config_for_path(self, path: list[object]) -> ConfigType:
        return self[path[0]][path[1]]


@contextlib.contextmanager
def full_config(hub_config: ConfigType) -> Iterator[None]:
    import esphome.final_validate as fv

    token = fv.full_config.set(FakeFullConfig(hub_config))
    try:
        yield
    finally:
        fv.full_config.reset(token)


@pytest.mark.parametrize(
    ("platform", "key"),
    [("number", "room_setpoint"), ("button", "hot_water_top_up"), ("switch", "room_sensor")],
)
def test_controls_need_room_sensor(
    thermia: ModuleType, platform: str, key: str
) -> None:
    module = importlib.import_module(f"components.thermia_itec_eco.{platform}")
    config = {"thermia_itec_eco_id": ID("hub"), key: {}}
    with full_config({"id": ID("hub")}), pytest.raises(cv.Invalid, match="room_sensor"):
        module.FINAL_VALIDATE_SCHEMA(config)
    with full_config({"id": ID("hub"), "room_sensor": {}}):
        module.FINAL_VALIDATE_SCHEMA(config)
    # A platform entry without the control needs nothing.
    with full_config({"id": ID("hub")}):
        module.FINAL_VALIDATE_SCHEMA({"thermia_itec_eco_id": ID("hub")})
