import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, DEVICE_CLASS_HEAT, DEVICE_CLASS_RUNNING
from esphome.types import ConfigType

from . import CONF_THERMIA_ITEC_ECO_ID, ThermiaItecEco, thermia_itec_eco_ns

DEPENDENCIES = ["thermia_itec_eco"]

BinarySensorType = thermia_itec_eco_ns.enum("BinarySensorType")

ICON_FAN = "mdi:fan"
ICON_HEAT_PUMP = "mdi:heat-pump"
ICON_PUMP = "mdi:pump"
ICON_SNOWFLAKE_MELT = "mdi:snowflake-melt"
ICON_VALVE = "mdi:valve"
ICON_WATER_BOILER = "mdi:water-boiler"
ICON_LIGHTNING_BOLT = "mdi:lightning-bolt"


def _running(icon: str) -> cv.Schema:
    return binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_RUNNING, icon=icon
    )


# Config key -> (BinarySensorType enum value, schema)
BINARY_SENSORS: dict[str, tuple[str, cv.Schema]] = {
    "compressor": ("BINARY_SENSOR_TYPE_COMPRESSOR", _running(ICON_HEAT_PUMP)),
    "defrost": ("BINARY_SENSOR_TYPE_DEFROST", _running(ICON_SNOWFLAKE_MELT)),
    "outdoor_fan": ("BINARY_SENSOR_TYPE_OUTDOOR_FAN", _running(ICON_FAN)),
    "circulation_pump": ("BINARY_SENSOR_TYPE_CIRCULATION_PUMP", _running(ICON_PUMP)),
    "hot_water_valve": ("BINARY_SENSOR_TYPE_HOT_WATER_VALVE", _running(ICON_VALVE)),
    "immersion_heater_1": (
        "BINARY_SENSOR_TYPE_IMMERSION_HEATER_1",
        binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_HEAT),
    ),
    "immersion_heater_2": (
        "BINARY_SENSOR_TYPE_IMMERSION_HEATER_2",
        binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_HEAT),
    ),
    "external_aux_heater": (
        "BINARY_SENSOR_TYPE_EXTERNAL_AUX_HEATER",
        binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_HEAT),
    ),
    "hot_water_top_up": (
        "BINARY_SENSOR_TYPE_HOT_WATER_TOP_UP",
        _running(ICON_WATER_BOILER),
    ),
    "high_power": (
        "BINARY_SENSOR_TYPE_HIGH_POWER",
        binary_sensor.binary_sensor_schema(icon=ICON_LIGHTNING_BOLT),
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
        cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
        **{cv.Optional(key): schema for key, (_, schema) in BINARY_SENSORS.items()},
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    for key, (sensor_type, _) in BINARY_SENSORS.items():
        if (sensor_config := config.get(key)) is not None:
            sens = await binary_sensor.new_binary_sensor(sensor_config)
            cg.add(
                hub.set_binary_sensor(getattr(BinarySensorType, sensor_type), sens)
            )
