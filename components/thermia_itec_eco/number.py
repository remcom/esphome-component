import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_ID, DEVICE_CLASS_TEMPERATURE, UNIT_CELSIUS
from esphome.types import ConfigType

from . import (
    CONF_THERMIA_ITEC_ECO_ID,
    ThermiaItecEco,
    require_room_sensor,
    thermia_itec_eco_ns,
)

DEPENDENCIES = ["thermia_itec_eco"]

CONF_ROOM_SETPOINT = "room_setpoint"

ICON_THERMOSTAT = "mdi:thermostat"

# Room setpoints the display accepts, °C (MIN/MAX_ROOM_SETPOINT in thermia_itec_eco.cpp).
MIN_ROOM_SETPOINT = 10
MAX_ROOM_SETPOINT = 30

RoomSetpointNumber = thermia_itec_eco_ns.class_("RoomSetpointNumber", number.Number)

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_THERMIA_ITEC_ECO_ID): cv.use_id(ThermiaItecEco),
    cv.Optional(CONF_ROOM_SETPOINT): number.number_schema(
        RoomSetpointNumber,
        device_class=DEVICE_CLASS_TEMPERATURE,
        unit_of_measurement=UNIT_CELSIUS,
        icon=ICON_THERMOSTAT,
    ),
}

FINAL_VALIDATE_SCHEMA = require_room_sensor(CONF_ROOM_SETPOINT)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_THERMIA_ITEC_ECO_ID])
    if (number_config := config.get(CONF_ROOM_SETPOINT)) is not None:
        num = await number.new_number(
            number_config,
            min_value=MIN_ROOM_SETPOINT,
            max_value=MAX_ROOM_SETPOINT,
            step=1,
        )
        await cg.register_parented(num, hub)
        cg.add(hub.set_room_setpoint_number(num))
