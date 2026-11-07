import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_RESPEAKER_LITE_ID, RespeakerLite

DEPENDENCIES = ["respeaker_lite"]

CONF_MIC_MUTE = "mic_mute"

ICON_MICROPHONE_OFF = "mdi:microphone-off"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RESPEAKER_LITE_ID): cv.use_id(RespeakerLite),
        cv.Optional(CONF_MIC_MUTE): binary_sensor.binary_sensor_schema(
            icon=ICON_MICROPHONE_OFF,
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_RESPEAKER_LITE_ID])
    binary_sensors = binary_sensor.sub_binary_sensors(config)
    await binary_sensors(CONF_MIC_MUTE, hub.set_mic_mute_binary_sensor)
