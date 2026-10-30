import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv
from esphome.const import CONF_ID, DEVICE_CLASS_MOTION
from esphome.types import ConfigType

from . import CONF_AQARA_FP2_ID, FP2Component

DEPENDENCIES = ["aqara_fp2"]

CONF_MOVEMENT = "movement"

# Movement events of the radar's motion report, by value (0, 1, 6, 7; MOVEMENT_EVENT_TYPES in
# aqara_fp2.cpp).
MOVEMENT_EVENT_TYPES = ["enter", "leave", "approach", "away"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
        cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
        cv.Optional(CONF_MOVEMENT): event.event_schema(
            event.Event, device_class=DEVICE_CLASS_MOTION
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_AQARA_FP2_ID])
    if (movement_config := config.get(CONF_MOVEMENT)) is not None:
        evt = await event.new_event(movement_config, event_types=MOVEMENT_EVENT_TYPES)
        cg.add(parent.set_movement_event(evt))
