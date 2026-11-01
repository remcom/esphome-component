import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import (
    CONF_AQARA_FP2_ACCEL_ID,
    CONF_AQARA_FP2_ID,
    FP2Accel,
    FP2Component,
    require_accel_for,
)

DEPENDENCIES = ["aqara_fp2"]

CONF_ORIENTATION = "orientation"
CONF_RADAR_VERSION = "radar_version"

ICON_CHIP = "mdi:chip"
ICON_SCREEN_ROTATION = "mdi:screen-rotation"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
            cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
            cv.Optional(CONF_AQARA_FP2_ACCEL_ID): cv.use_id(FP2Accel),
            cv.Optional(CONF_RADAR_VERSION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=ICON_CHIP,
            ),
            cv.Optional(CONF_ORIENTATION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=ICON_SCREEN_ROTATION,
            ),
        }
    ),
    require_accel_for(CONF_ORIENTATION),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AQARA_FP2_ID])
    text_sensors = text_sensor.sub_text_sensors(config)
    await text_sensors(CONF_RADAR_VERSION, hub.set_radar_version_text_sensor)
    if (accel_id := config.get(CONF_AQARA_FP2_ACCEL_ID)) is not None:
        accel = await cg.get_variable(accel_id)
        await text_sensors(CONF_ORIENTATION, accel.set_orientation_text_sensor)
