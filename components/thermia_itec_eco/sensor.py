import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_PRESSURE,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_PERCENT,
    UNIT_REVOLUTIONS_PER_MINUTE,
)
from esphome.types import ConfigType

from . import CONF_THERMIA_ITEC_ECO_ID, ThermiaItecEco, thermia_itec_eco_ns

DEPENDENCIES = ["thermia_itec_eco"]

SensorType = thermia_itec_eco_ns.enum("SensorType")

UNIT_BAR = "bar"
UNIT_KELVIN = "K"
UNIT_STEPS = "steps"

ICON_FAN = "mdi:fan"
ICON_HEAT_PUMP = "mdi:heat-pump"
ICON_PUMP = "mdi:pump"
ICON_THERMOSTAT = "mdi:thermostat"
ICON_VALVE = "mdi:valve"


def _temperature(decimals: int = 0, **kwargs) -> cv.Schema:
    return sensor.sensor_schema(
        device_class=DEVICE_CLASS_TEMPERATURE,
        unit_of_measurement=UNIT_CELSIUS,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=decimals,
        **kwargs,
    )


def _setting(unit: str | None = None, **kwargs) -> cv.Schema:
    """A setting published by the display (read-only here)."""
    if unit is not None:
        kwargs["unit_of_measurement"] = unit
    return sensor.sensor_schema(
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon=ICON_THERMOSTAT,
        **kwargs,
    )


# Config key -> (SensorType enum value, schema)
SENSORS: dict[str, tuple[str, cv.Schema]] = {
    "supply_temperature": ("SENSOR_TYPE_SUPPLY_TEMPERATURE", _temperature()),
    "outdoor_temperature": ("SENSOR_TYPE_OUTDOOR_TEMPERATURE", _temperature()),
    "hot_water_top_temperature": (
        "SENSOR_TYPE_HOT_WATER_TOP_TEMPERATURE",
        _temperature(),
    ),
    "hot_water_temperature": ("SENSOR_TYPE_HOT_WATER_TEMPERATURE", _temperature()),
    "room_temperature": ("SENSOR_TYPE_ROOM_TEMPERATURE", _temperature(1)),
    "room_setpoint": (
        "SENSOR_TYPE_ROOM_SETPOINT",
        _temperature(icon=ICON_THERMOSTAT),
    ),
    "target_supply_temperature": (
        "SENSOR_TYPE_TARGET_SUPPLY_TEMPERATURE",
        _temperature(1),
    ),
    "condenser_inlet_temperature": (
        "SENSOR_TYPE_CONDENSER_INLET_TEMPERATURE",
        _temperature(1),
    ),
    "condenser_outlet_temperature": (
        "SENSOR_TYPE_CONDENSER_OUTLET_TEMPERATURE",
        _temperature(1),
    ),
    "high_pressure": (
        "SENSOR_TYPE_HIGH_PRESSURE",
        sensor.sensor_schema(
            device_class=DEVICE_CLASS_PRESSURE,
            unit_of_measurement=UNIT_BAR,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=1,
        ),
    ),
    "compressor_frequency": (
        "SENSOR_TYPE_COMPRESSOR_FREQUENCY",
        sensor.sensor_schema(
            device_class=DEVICE_CLASS_FREQUENCY,
            unit_of_measurement=UNIT_HERTZ,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
            icon=ICON_HEAT_PUMP,
        ),
    ),
    "compressor_current": (
        "SENSOR_TYPE_COMPRESSOR_CURRENT",
        sensor.sensor_schema(
            device_class=DEVICE_CLASS_CURRENT,
            unit_of_measurement=UNIT_AMPERE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=1,
        ),
    ),
    "outdoor_fan_speed": (
        "SENSOR_TYPE_OUTDOOR_FAN_SPEED",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
            icon=ICON_FAN,
        ),
    ),
    "expansion_valve_position": (
        "SENSOR_TYPE_EXPANSION_VALVE_POSITION",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_STEPS,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
            icon=ICON_VALVE,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    "circulation_pump_speed": (
        "SENSOR_TYPE_CIRCULATION_PUMP_SPEED",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
            icon=ICON_PUMP,
        ),
    ),
    "heating_curve": ("SENSOR_TYPE_HEATING_CURVE", _setting()),
    "heating_curve_min": ("SENSOR_TYPE_HEATING_CURVE_MIN", _setting(UNIT_CELSIUS)),
    "heating_curve_max": ("SENSOR_TYPE_HEATING_CURVE_MAX", _setting(UNIT_CELSIUS)),
    "curve_correction_plus_5": (
        "SENSOR_TYPE_CURVE_CORRECTION_PLUS_5",
        _setting(UNIT_KELVIN),
    ),
    "curve_correction_0": ("SENSOR_TYPE_CURVE_CORRECTION_0", _setting(UNIT_KELVIN)),
    "curve_correction_minus_5": (
        "SENSOR_TYPE_CURVE_CORRECTION_MINUS_5",
        _setting(UNIT_KELVIN),
    ),
    "heating_stop_temperature": (
        "SENSOR_TYPE_HEATING_STOP_TEMPERATURE",
        _setting(UNIT_CELSIUS),
    ),
    "setback_temperature": (
        "SENSOR_TYPE_SETBACK_TEMPERATURE",
        _setting(UNIT_CELSIUS),
    ),
    "room_factor": ("SENSOR_TYPE_ROOM_FACTOR", _setting()),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
        cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
        **{cv.Optional(key): schema for key, (_, schema) in SENSORS.items()},
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    for key, (sensor_type, _) in SENSORS.items():
        if (sensor_config := config.get(key)) is not None:
            sens = await sensor.new_sensor(sensor_config)
            cg.add(hub.set_sensor(getattr(SensorType, sensor_type), sens))
