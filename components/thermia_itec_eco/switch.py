import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG
from esphome.types import ConfigType

from . import (
    CONF_ROOM_SENSOR,
    CONF_THERMIA_ITEC_ECO_ID,
    ThermiaItecEco,
    require_room_sensor,
    thermia_itec_eco_ns,
)

DEPENDENCIES = ["thermia_itec_eco"]

ICON_THERMOMETER_CHECK = "mdi:thermometer-check"

RoomSensorSwitch = thermia_itec_eco_ns.class_("RoomSensorSwitch", switch.Switch)

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
    # Off after a fresh flash: nothing is transmitted until the switch is turned on.
    cv.Optional(CONF_ROOM_SENSOR): switch.switch_schema(
        RoomSensorSwitch,
        default_restore_mode="RESTORE_DEFAULT_OFF",
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon=ICON_THERMOMETER_CHECK,
    ),
}

FINAL_VALIDATE_SCHEMA = require_room_sensor(CONF_ROOM_SENSOR)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    switches = switch.sub_switches(config, parent=hub)
    await switches(CONF_ROOM_SENSOR, hub.set_room_sensor_switch)
