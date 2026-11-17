import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

from . import (
    CONF_THERMIA_ITEC_ECO_ID,
    ThermiaItecEco,
    require_room_sensor,
    thermia_itec_eco_ns,
)

DEPENDENCIES = ["thermia_itec_eco"]

CONF_HOT_WATER_TOP_UP = "hot_water_top_up"

ICON_WATER_BOILER = "mdi:water-boiler"

HotWaterTopUpButton = thermia_itec_eco_ns.class_("HotWaterTopUpButton", button.Button)

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
    cv.Optional(CONF_HOT_WATER_TOP_UP): button.button_schema(
        HotWaterTopUpButton, icon=ICON_WATER_BOILER
    ),
}

FINAL_VALIDATE_SCHEMA = require_room_sensor(CONF_HOT_WATER_TOP_UP)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    buttons = button.sub_buttons(config, parent=hub)
    await buttons(CONF_HOT_WATER_TOP_UP, hub.set_hot_water_top_up_button)
