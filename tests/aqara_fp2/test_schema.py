"""Schema validation tests — run without hardware, requires esphome installed."""

from collections.abc import Callable
import importlib
from types import ModuleType

import pytest

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.types import ConfigType

EMPTY_ROW = ". " * 13 + "."
EMPTY_GRID = "\n".join([EMPTY_ROW] * 14)


def grid_with(*cells: tuple[int, int]) -> str:
    """A 14x14 grid with the given (row, col) cells active."""
    rows = [["."] * 14 for _ in range(14)]
    for row, col in cells:
        rows[row][col] = "X"
    return "\n".join(" ".join(row) for row in rows)


@pytest.fixture
def aqara_fp2(load_component: Callable[[str], ModuleType]) -> ModuleType:
    return load_component("aqara_fp2")


@pytest.fixture
def schema(aqara_fp2: ModuleType) -> Callable[[ConfigType], ConfigType]:
    return aqara_fp2.CONFIG_SCHEMA


@pytest.fixture
def binary_sensor_platform(aqara_fp2: ModuleType) -> ModuleType:
    return importlib.import_module("components.aqara_fp2.binary_sensor")


def test_minimal_config_accepted(schema: Callable[[ConfigType], ConfigType]) -> None:
    result = schema({})
    assert result["mounting_position"].enum_value == 0x01
    assert result["presence_sensitivity"].enum_value == 2
    assert result["left_right_reverse"] is False
    assert result["radar_timeout"].total_milliseconds == 60000
    assert result["motion_timeout"].total_milliseconds == 30000
    assert "accel" not in result


def test_radar_timeout_zero_disables(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    assert schema({"radar_timeout": "0s"})["radar_timeout"].total_milliseconds == 0


def test_accel_defaults(schema: Callable[[ConfigType], ConfigType]) -> None:
    accel = schema({"accel": {}})["accel"]
    assert accel["address"] == 0x27


def test_accel_has_no_update_interval(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    """The sample interval is fixed: the stock firmware's debounce counts depend on it."""
    with pytest.raises(cv.Invalid):
        schema({"accel": {"update_interval": "1s"}})


def test_grid_bit_layout(aqara_fp2: ModuleType) -> None:
    """Input columns 0..13 map to bits 13..0 (2-column offset); each row is 2 big-endian bytes."""
    grid = aqara_fp2.parse_ascii_grid(grid_with((0, 0), (1, 13)))
    assert len(grid) == 40
    assert grid[0:2] == [0x20, 0x00]
    assert grid[2:4] == [0x00, 0x01]
    assert not any(grid[4:])


def test_grid_accepts_dash_and_no_spaces(aqara_fp2: ModuleType) -> None:
    rows = ["X" + "-" * 13] + ["." * 14] * 13
    assert aqara_fp2.parse_ascii_grid("\n".join(rows))[0:2] == [0x20, 0x00]


@pytest.mark.parametrize(
    ("grid", "match"),
    [
        ("\n".join([EMPTY_ROW] * 13), "exactly 14 rows"),
        ("\n".join([EMPTY_ROW] * 13 + [EMPTY_ROW + " ."]), "Row 14 must have 14"),
        ("\n".join([EMPTY_ROW.replace(".", "o", 1)] + [EMPTY_ROW] * 13), "'o'"),
    ],
)
def test_invalid_grid_rejected(aqara_fp2: ModuleType, grid: str, match: str) -> None:
    with pytest.raises(cv.Invalid, match=match):
        aqara_fp2.parse_ascii_grid(grid)


def test_zone_defaults(schema: Callable[[ConfigType], ConfigType]) -> None:
    zone = schema({"zones": [{"grid": EMPTY_GRID}]})["zones"][0]
    assert zone["sensitivity"].enum_value == 2


def test_zone_entities_not_on_hub(schema: Callable[[ConfigType], ConfigType]) -> None:
    """Zone entities belong on the binary_sensor platform (zone_<n>), not under zones."""
    with pytest.raises(cv.Invalid):
        schema({"zones": [{"grid": EMPTY_GRID, "presence": {"name": "Sofa"}}]})


def test_zone_count_limited(
    schema: Callable[[ConfigType], ConfigType], aqara_fp2: ModuleType
) -> None:
    """Zone ids index the radar's 32-entry activation list."""
    zones = [{"grid": EMPTY_GRID}] * aqara_fp2.MAX_ZONES
    assert schema({"zones": zones})
    with pytest.raises(cv.Invalid):
        schema({"zones": [*zones, {"grid": EMPTY_GRID}]})


def test_require_accel_for(aqara_fp2: ModuleType) -> None:
    """Accelerometer entities resolve the accelerometer ID; radar-only entries do not."""
    validate = aqara_fp2.require_accel_for("vibration")
    assert aqara_fp2.CONF_AQARA_FP2_ACCEL_ID not in validate({"presence": {}})
    result = validate({"vibration": {}})
    assert aqara_fp2.CONF_AQARA_FP2_ACCEL_ID in result


def _final_validate(
    platform: ModuleType, full_config: dict, config: ConfigType
) -> ConfigType:
    token = fv.full_config.set(full_config)
    try:
        return platform.FINAL_VALIDATE_SCHEMA(config)
    finally:
        fv.full_config.reset(token)


def test_zone_entities_need_a_defined_zone(binary_sensor_platform: ModuleType) -> None:
    full_config = {"aqara_fp2": {"zones": [{"grid": []}]}}
    assert _final_validate(binary_sensor_platform, full_config, {"zone_1": {}})
    with pytest.raises(cv.Invalid, match="zone_2"):
        _final_validate(binary_sensor_platform, full_config, {"zone_2": {}})
