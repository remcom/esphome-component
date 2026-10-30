"""
Schema validation tests — run without hardware, requires esphome installed.
Run: pytest tests/grid_meter/test_schema.py -v
"""

from collections.abc import Callable
from types import ModuleType

import pytest

import esphome.config_validation as cv
from esphome.types import ConfigType

REQUIRED = {
    "power_import": "power_delivered",
    "power_export": "power_returned",
    "voltage": "voltage_l1",
    "current": "current_l1",
    "energy_import_t1": "energy_delivered_tariff1",
    "energy_import_t2": "energy_delivered_tariff2",
    "energy_export_t1": "energy_returned_tariff1",
    "energy_export_t2": "energy_returned_tariff2",
}

PHASE_L2 = {
    "voltage_l2": "v2",
    "current_l2": "c2",
    "power_import_l2": "pi2",
    "power_export_l2": "pe2",
}


@pytest.fixture
def schema(
    load_component: Callable[[str], ModuleType],
) -> Callable[[ConfigType], ConfigType]:
    return load_component("grid_meter").CONFIG_SCHEMA


def test_all_sensor_keys_are_required(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    """CONFIG_SCHEMA must declare all eight sensor keys as Required."""
    for key in REQUIRED:
        partial = {k: v for k, v in REQUIRED.items() if k != key}
        with pytest.raises(cv.Invalid, match=key):
            schema(partial)


def test_valid_config_accepted(schema: Callable[[ConfigType], ConfigType]) -> None:
    """CONFIG_SCHEMA must accept the eight sensor keys and fill in defaults."""
    result = schema(dict(REQUIRED))
    assert result["port"] == 502
    assert result["data_timeout"].total_milliseconds == 30000


def test_phase_group_all_or_none(schema: Callable[[ConfigType], ConfigType]) -> None:
    """An L2 group with only some of its keys must be rejected."""
    assert schema({**REQUIRED, **PHASE_L2})
    partial = {k: v for k, v in PHASE_L2.items() if k != "current_l2"}
    with pytest.raises(cv.Invalid):
        schema({**REQUIRED, **partial})


def test_serial_number_limits(schema: Callable[[ConfigType], ConfigType]) -> None:
    """serial_number must be ASCII and fit the 7 Reg_text registers (14 chars)."""
    assert schema({**REQUIRED, "serial_number": "A" * 14})
    with pytest.raises(cv.Invalid):
        schema({**REQUIRED, "serial_number": "A" * 15})
    with pytest.raises(cv.Invalid, match="ASCII"):
        schema({**REQUIRED, "serial_number": "Zähler"})


def test_data_timeout_zero_disables(schema: Callable[[ConfigType], ConfigType]) -> None:
    """data_timeout: 0s is accepted (disables the watchdog)."""
    assert (
        schema({**REQUIRED, "data_timeout": "0s"})["data_timeout"].total_milliseconds
        == 0
    )
