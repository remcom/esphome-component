import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_SPEED,
    CONF_X,
    CONF_Y,
    DEVICE_CLASS_DISTANCE,
    DEVICE_CLASS_SPEED,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_DEGREES,
    UNIT_MILLIMETER,
)
from esphome.types import ConfigType

from . import (
    CONF_AQARA_FP2_ACCEL_ID,
    CONF_AQARA_FP2_ID,
    FP2Accel,
    FP2Component,
    require_accel_for,
)

DEPENDENCIES = ["aqara_fp2"]

CONF_TARGET_COUNT = "target_count"
CONF_RADAR_TEMPERATURE = "radar_temperature"
CONF_TILT_ANGLE = "tilt_angle"
CONF_Z = "z"

MAX_TARGETS = 5

UNIT_MILLIMETER_PER_SECOND = "mm/s"

ICON_ACCOUNT_GROUP = "mdi:account-group"
ICON_ANGLE_ACUTE = "mdi:angle-acute"
ICON_ALPHA_X_BOX_OUTLINE = "mdi:alpha-x-box-outline"
ICON_ALPHA_Y_BOX_OUTLINE = "mdi:alpha-y-box-outline"
ICON_ALPHA_Z_BOX_OUTLINE = "mdi:alpha-z-box-outline"
ICON_SPEEDOMETER_SLOW = "mdi:speedometer-slow"


def _target_schema() -> cv.Schema:
    return cv.Schema(
        {
            cv.Optional(CONF_X): sensor.sensor_schema(
                device_class=DEVICE_CLASS_DISTANCE,
                unit_of_measurement=UNIT_MILLIMETER,
                icon=ICON_ALPHA_X_BOX_OUTLINE,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_Y): sensor.sensor_schema(
                device_class=DEVICE_CLASS_DISTANCE,
                unit_of_measurement=UNIT_MILLIMETER,
                icon=ICON_ALPHA_Y_BOX_OUTLINE,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_Z): sensor.sensor_schema(
                device_class=DEVICE_CLASS_DISTANCE,
                unit_of_measurement=UNIT_MILLIMETER,
                icon=ICON_ALPHA_Z_BOX_OUTLINE,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_SPEED): sensor.sensor_schema(
                device_class=DEVICE_CLASS_SPEED,
                unit_of_measurement=UNIT_MILLIMETER_PER_SECOND,
                icon=ICON_SPEEDOMETER_SLOW,
                accuracy_decimals=0,
            ),
        }
    )


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
            cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
            cv.Optional(CONF_AQARA_FP2_ACCEL_ID): cv.use_id(FP2Accel),
            cv.Optional(CONF_TARGET_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                icon=ICON_ACCOUNT_GROUP,
            ),
            cv.Optional(CONF_RADAR_TEMPERATURE): sensor.sensor_schema(
                device_class=DEVICE_CLASS_TEMPERATURE,
                unit_of_measurement=UNIT_CELSIUS,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
            ),
            cv.Optional(CONF_TILT_ANGLE): sensor.sensor_schema(
                unit_of_measurement=UNIT_DEGREES,
                accuracy_decimals=0,
                icon=ICON_ANGLE_ACUTE,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(
        {cv.Optional(f"target_{n + 1}"): _target_schema() for n in range(MAX_TARGETS)}
    ),
    require_accel_for(CONF_TILT_ANGLE),
)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_AQARA_FP2_ID])

    if (target_count_config := config.get(CONF_TARGET_COUNT)) is not None:
        sens = await sensor.new_sensor(target_count_config)
        cg.add(parent.set_target_count_sensor(sens))
    if (radar_temperature_config := config.get(CONF_RADAR_TEMPERATURE)) is not None:
        sens = await sensor.new_sensor(radar_temperature_config)
        cg.add(parent.set_radar_temperature_sensor(sens))
    if (tilt_angle_config := config.get(CONF_TILT_ANGLE)) is not None:
        accel = await cg.get_variable(config[CONF_AQARA_FP2_ACCEL_ID])
        sens = await sensor.new_sensor(tilt_angle_config)
        cg.add(accel.set_tilt_angle_sensor(sens))

    for n in range(MAX_TARGETS):
        if (target_config := config.get(f"target_{n + 1}")) is not None:
            if (x_config := target_config.get(CONF_X)) is not None:
                cg.add(parent.set_target_x_sensor(n, await sensor.new_sensor(x_config)))
            if (y_config := target_config.get(CONF_Y)) is not None:
                cg.add(parent.set_target_y_sensor(n, await sensor.new_sensor(y_config)))
            if (z_config := target_config.get(CONF_Z)) is not None:
                cg.add(parent.set_target_z_sensor(n, await sensor.new_sensor(z_config)))
            if (speed_config := target_config.get(CONF_SPEED)) is not None:
                cg.add(
                    parent.set_target_speed_sensor(
                        n, await sensor.new_sensor(speed_config)
                    )
                )
