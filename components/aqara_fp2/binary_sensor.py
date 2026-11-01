import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_HAS_TARGET,
    CONF_ID,
    CONF_MOTION,
    DEVICE_CLASS_MOTION,
    DEVICE_CLASS_OCCUPANCY,
    DEVICE_CLASS_VIBRATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import (
    CONF_AQARA_FP2_ACCEL_ID,
    CONF_AQARA_FP2_ID,
    CONF_PRESENCE,
    CONF_ZONES,
    DOMAIN,
    MAX_ZONES,
    FP2Accel,
    FP2Component,
    require_accel_for,
)

DEPENDENCIES = ["aqara_fp2"]

CONF_VIBRATION = "vibration"


def _zone_key(index: int) -> str:
    """Config key of the zone at 0-based `index` (zones are numbered from 1)."""
    return f"zone_{index + 1}"


ZONE_ENTITIES_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_OCCUPANCY,
        ),
        cv.Optional(CONF_MOTION): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_MOTION,
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
            cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
            cv.Optional(CONF_AQARA_FP2_ACCEL_ID): cv.use_id(FP2Accel),
            cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_OCCUPANCY,
            ),
            cv.Optional(CONF_MOTION): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_MOTION,
            ),
            cv.Optional(CONF_HAS_TARGET): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_OCCUPANCY,
            ),
            cv.Optional(CONF_VIBRATION): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_VIBRATION,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            **{
                cv.Optional(_zone_key(index)): ZONE_ENTITIES_SCHEMA
                for index in range(MAX_ZONES)
            },
        }
    ),
    require_accel_for(CONF_VIBRATION),
)


def _final_validate(config: ConfigType) -> None:
    zone_count = len(fv.full_config.get().get(DOMAIN, {}).get(CONF_ZONES, []))
    for index in range(zone_count, MAX_ZONES):
        if _zone_key(index) in config:
            raise cv.Invalid(
                f"{_zone_key(index)} refers to a zone that is not defined under "
                f"{DOMAIN}: zones ({zone_count} defined)",
                path=[_zone_key(index)],
            )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AQARA_FP2_ID])
    binary_sensors = binary_sensor.sub_binary_sensors(config)
    await binary_sensors(CONF_PRESENCE, hub.set_presence_binary_sensor)
    await binary_sensors(CONF_MOTION, hub.set_motion_binary_sensor)
    await binary_sensors(CONF_HAS_TARGET, hub.set_target_binary_sensor)
    if (accel_id := config.get(CONF_AQARA_FP2_ACCEL_ID)) is not None:
        accel = await cg.get_variable(accel_id)
        await binary_sensors(CONF_VIBRATION, accel.set_vibration_binary_sensor)

    for index in range(MAX_ZONES):
        if (zone_config := config.get(_zone_key(index))) is None:
            continue
        if (presence_config := zone_config.get(CONF_PRESENCE)) is not None:
            sens = await binary_sensor.new_binary_sensor(presence_config)
            cg.add(hub.set_zone_presence_binary_sensor(index, sens))
        if (motion_config := zone_config.get(CONF_MOTION)) is not None:
            sens = await binary_sensor.new_binary_sensor(motion_config)
            cg.add(hub.set_zone_motion_binary_sensor(index, sens))
