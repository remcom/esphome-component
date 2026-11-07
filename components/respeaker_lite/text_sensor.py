import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import CONF_RESPEAKER_LITE_ID, RespeakerLite

DEPENDENCIES = ["respeaker_lite"]

CONF_FIRMWARE_VERSION = "firmware_version"

ICON_CHIP = "mdi:chip"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RESPEAKER_LITE_ID): cv.use_id(RespeakerLite),
        cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_CHIP,
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_RESPEAKER_LITE_ID])
    text_sensors = text_sensor.sub_text_sensors(config)
    await text_sensors(CONF_FIRMWARE_VERSION, hub.set_firmware_version_text_sensor)
