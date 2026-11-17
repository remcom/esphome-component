import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

from . import CONF_THERMIA_ITEC_ECO_ID, ThermiaItecEco

DEPENDENCIES = ["thermia_itec_eco"]

CONF_OPERATING_MODE = "operating_mode"

ICON_HEAT_PUMP = "mdi:heat-pump"

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
    cv.Optional(CONF_OPERATING_MODE): text_sensor.text_sensor_schema(
        icon=ICON_HEAT_PUMP
    ),
}


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    text_sensors = text_sensor.sub_text_sensors(config)
    await text_sensors(CONF_OPERATING_MODE, hub.set_operating_mode_text_sensor)
