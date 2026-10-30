"""Schema validation tests — run without hardware, requires esphome installed."""

from collections.abc import Callable
from types import ModuleType

import pytest

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.types import ConfigType

MINIMAL = {
    "manufacturer": "Solis",
    "model": "S6-GR1P3K-M",
    "rated_power": 3000,
    "ac_power": "ac_power",
    "ac_voltage": "ac_voltage",
    "ac_frequency": "ac_frequency",
    "temperature": "inverter_temp",
}


@pytest.fixture
def sunspec(load_component: Callable[[str], ModuleType]) -> ModuleType:
    return load_component("sunspec")


@pytest.fixture
def schema(sunspec: ModuleType) -> Callable[[ConfigType], ConfigType]:
    return sunspec.CONFIG_SCHEMA


def test_minimal_config_accepted(schema: Callable[[ConfigType], ConfigType]) -> None:
    result = schema(dict(MINIMAL))
    assert result["port"] == 502
    assert result["phases"] == 1
    assert result["serial_number"] == ""


@pytest.mark.parametrize("rated_power", [0, -1, 2_000_001])
def test_rated_power_out_of_range(
    schema: Callable[[ConfigType], ConfigType], rated_power: int
) -> None:
    """rated_power must be at least 1 W (WRtg 0 breaks limit calculations)."""
    with pytest.raises(cv.Invalid):
        schema({**MINIMAL, "rated_power": rated_power})


@pytest.mark.parametrize(
    ("rated_power", "sf"), [(1, 0), (3000, 0), (21844, 0), (21845, 1), (2_000_000, 2)]
)
def test_power_scale_factor(sunspec: ModuleType, rated_power: int, sf: int) -> None:
    """1.5x rated power must fit int16 at the chosen scale factor."""
    assert sunspec.power_scale_factor(rated_power) == sf
    assert rated_power * 1.5 / 10**sf <= 32767


def test_strings_must_be_ascii(schema: Callable[[ConfigType], ConfigType]) -> None:
    with pytest.raises(cv.Invalid, match="ASCII"):
        schema({**MINIMAL, "manufacturer": "Sólis"})


def test_string_length_limits(schema: Callable[[ConfigType], ConfigType]) -> None:
    assert schema({**MINIMAL, "model": "M" * 32})
    with pytest.raises(cv.Invalid):
        schema({**MINIMAL, "model": "M" * 33})
    with pytest.raises(cv.Invalid):
        schema({**MINIMAL, "version": "V" * 17})


def test_phase_sensor_requires_three_phases(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    with pytest.raises(cv.Invalid, match="phases: 3"):
        schema({**MINIMAL, "ac_current_phase_b": "current_l2"})
    assert schema({**MINIMAL, "phases": 3, "ac_current_phase_b": "current_l2"})


def test_modbus_write_back_keys_are_inclusive(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    """modbus_controller_id and power_limit_register must appear together."""
    with pytest.raises(cv.Invalid):
        schema({**MINIMAL, "modbus_controller_id": "modbus_master"})
    with pytest.raises(cv.Invalid):
        schema({**MINIMAL, "power_limit_register": 3051})
    assert schema(
        {
            **MINIMAL,
            "modbus_controller_id": "modbus_master",
            "power_limit_register": 3051,
        }
    )


def test_power_limit_register_is_uint16(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    with pytest.raises(cv.Invalid):
        schema(
            {
                **MINIMAL,
                "modbus_controller_id": "modbus_master",
                "power_limit_register": 65536,
            }
        )


def test_only_one_write_back_path(schema: Callable[[ConfigType], ConfigType]) -> None:
    """The number entity and direct register write-back are mutually exclusive."""
    with pytest.raises(cv.Invalid):
        schema(
            {
                **MINIMAL,
                "power_limit_number_id": "power_limit",
                "modbus_controller_id": "modbus_master",
                "power_limit_register": 3051,
            }
        )


def _final_validate(sunspec: ModuleType, full_config: dict, config: dict) -> dict:
    token = fv.full_config.set(full_config)
    try:
        return sunspec.FINAL_VALIDATE_SCHEMA(config)
    finally:
        fv.full_config.reset(token)


def test_duplicate_sunspec_port_rejected(sunspec: ModuleType) -> None:
    a, b = {"port": 502}, {"port": 502}
    with pytest.raises(cv.Invalid, match="unique port"):
        _final_validate(sunspec, {"sunspec": [a, b]}, a)
    c = {"port": 1502}
    assert _final_validate(sunspec, {"sunspec": [a, c]}, a)


def test_port_clash_with_grid_meter_rejected(sunspec: ModuleType) -> None:
    conf = {"port": 502}
    with pytest.raises(cv.Invalid, match="grid_meter"):
        _final_validate(sunspec, {"sunspec": [conf], "grid_meter": {"port": 502}}, conf)
    assert _final_validate(
        sunspec, {"sunspec": [conf], "grid_meter": {"port": 5020}}, conf
    )
