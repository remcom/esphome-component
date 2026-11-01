import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ID, DEVICE_CLASS_RESTART, ENTITY_CATEGORY_CONFIG
from esphome.types import ConfigType

from . import CONF_AQARA_FP2_ID, FP2Component, aqara_fp2_ns

DEPENDENCIES = ["aqara_fp2"]

CONF_RESTART_RADAR = "restart_radar"

RestartRadarButton = aqara_fp2_ns.class_("RestartRadarButton", button.Button)

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
    cv.Optional(CONF_RESTART_RADAR): button.button_schema(
        RestartRadarButton,
        device_class=DEVICE_CLASS_RESTART,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ),
}


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AQARA_FP2_ID])
    buttons = button.sub_buttons(config, parent=hub)
    await buttons(CONF_RESTART_RADAR, hub.set_restart_radar_button)
