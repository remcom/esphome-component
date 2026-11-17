import esphome.codegen as cg
from esphome.components import sensor, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TEMPERATURE, CONF_TIMEOUT
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@remcom"]
DEPENDENCIES = ["uart"]
DOMAIN = "thermia_itec_eco"

thermia_itec_eco_ns = cg.esphome_ns.namespace("thermia_itec_eco")
ThermiaItecEco = thermia_itec_eco_ns.class_(
    "ThermiaItecEco", cg.Component, uart.UARTDevice
)

CONF_THERMIA_ITEC_ECO_ID = "thermia_itec_eco_id"
CONF_BUS_TIMEOUT = "bus_timeout"
CONF_ROOM_SENSOR = "room_sensor"

# The display panel is the bus master: Modbus RTU at 9600 baud, 8E1.
BUS_BAUD_RATE = 9600

ROOM_SENSOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_TIMEOUT, default="1h"): cv.positive_time_period_milliseconds,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ThermiaItecEco),
            cv.Optional(
                CONF_BUS_TIMEOUT, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ROOM_SENSOR): ROOM_SENSOR_SCHEMA,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.require_esphome_version(2026, 10, 0),
)


def _final_validate(config: ConfigType) -> None:
    uart.final_validate_device_schema(
        DOMAIN,
        baud_rate=BUS_BAUD_RATE,
        require_rx=True,
        # Only the room sensor emulation transmits; without it the component only listens.
        require_tx=CONF_ROOM_SENSOR in config,
        data_bits=8,
        parity="EVEN",
        stop_bits=1,
    )(config)


FINAL_VALIDATE_SCHEMA = _final_validate


def require_room_sensor(*keys: str):
    """Platform final validator: `keys` only work when the hub emulates the room sensor."""

    def validator(config: ConfigType) -> None:
        if not any(key in config for key in keys):
            return
        full_config = fv.full_config.get()
        hub_path = full_config.get_path_for_id(config[CONF_THERMIA_ITEC_ECO_ID])[:-1]
        if CONF_ROOM_SENSOR not in full_config.get_config_for_path(hub_path):
            key = next(key for key in keys if key in config)
            raise cv.Invalid(
                f"{key} needs room sensor emulation: add a {CONF_ROOM_SENSOR} block "
                f"to {DOMAIN}",
                path=[key],
            )

    return validator


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_bus_timeout(config[CONF_BUS_TIMEOUT].total_milliseconds))
    if (room_config := config.get(CONF_ROOM_SENSOR)) is not None:
        cg.add_define("USE_THERMIA_ITEC_ECO_ROOM_SENSOR")
        temperature = await cg.get_variable(room_config[CONF_TEMPERATURE])
        cg.add(
            var.set_room_sensor(
                temperature, room_config[CONF_TIMEOUT].total_milliseconds
            )
        )
