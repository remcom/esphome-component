from esphome import automation
import esphome.codegen as cg
from esphome.components import modbus_controller, number, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_MAX_CONNECTIONS,
    CONF_MODEL,
    CONF_PORT,
    CONF_TEMPERATURE,
    CONF_VERSION,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

AUTO_LOAD = ["sensor", "socket"]
CODEOWNERS = ["@remcom"]
DEPENDENCIES = ["network"]
DOMAIN = "sunspec"
MULTI_CONF = True

sunspec_ns = cg.esphome_ns.namespace("sunspec")
SunspecComponent = sunspec_ns.class_("SunspecComponent", cg.Component)

CONF_MANUFACTURER = "manufacturer"
CONF_SERIAL_NUMBER = "serial_number"
CONF_RATED_POWER = "rated_power"
CONF_STALE_TIMEOUT = "stale_timeout"
CONF_PHASES = "phases"
CONF_AC_POWER = "ac_power"
CONF_AC_VOLTAGE = "ac_voltage"
CONF_AC_CURRENT = "ac_current"
CONF_AC_CURRENT_PHASE_A = "ac_current_phase_a"
CONF_AC_CURRENT_PHASE_B = "ac_current_phase_b"
CONF_AC_CURRENT_PHASE_C = "ac_current_phase_c"
CONF_AC_VOLTAGE_PHASE_B = "ac_voltage_phase_b"
CONF_AC_VOLTAGE_PHASE_C = "ac_voltage_phase_c"
CONF_AC_FREQUENCY = "ac_frequency"
CONF_ENERGY_TOTAL = "energy_total"
CONF_DC_POWER = "dc_power"
CONF_DC_VOLTAGE = "dc_voltage"
CONF_DC_CURRENT = "dc_current"
CONF_MODBUS_CONTROLLER_ID = "modbus_controller_id"
CONF_POWER_LIMIT_REGISTER = "power_limit_register"
CONF_POWER_LIMIT_NUMBER_ID = "power_limit_number_id"
CONF_ON_POWER_LIMIT = "on_power_limit"

# Largest rated power that still fits the 16-bit power registers at the maximum scale factor
MAX_RATED_POWER = 2_000_000

PHASE_SENSOR_KEYS = [
    CONF_AC_CURRENT_PHASE_A,
    CONF_AC_CURRENT_PHASE_B,
    CONF_AC_CURRENT_PHASE_C,
    CONF_AC_VOLTAGE_PHASE_B,
    CONF_AC_VOLTAGE_PHASE_C,
]


def ascii_string(value: object) -> str:
    """Validate a string that is packed byte-wise into SunSpec registers."""
    value = cv.string_strict(value)
    if not value.isascii():
        raise cv.Invalid("must contain only ASCII characters")
    return value


def power_scale_factor(rated_power: int) -> int:
    """Smallest scale factor keeping 1.5x rated power (DC oversizing) within int16."""
    for sf in range(3):
        if rated_power * 1.5 / 10**sf <= 32767:
            return sf
    raise ValueError(f"rated_power {rated_power} exceeds {MAX_RATED_POWER}")


def _validate_phases(config: ConfigType) -> ConfigType:
    if config[CONF_PHASES] == 1:
        for key in PHASE_SENSOR_KEYS:
            if key in config:
                raise cv.Invalid(f"'{key}' requires 'phases: 3'")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SunspecComponent),
            cv.Optional(CONF_PORT, default=502): cv.port,
            cv.Optional(CONF_MAX_CONNECTIONS, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_ADDRESS, default=1): cv.int_range(min=1, max=247),
            # After this long without a sensor update, its registers report
            # "not implemented" and the inverter state falls back to Off (0 disables)
            cv.Optional(
                CONF_STALE_TIMEOUT, default="5min"
            ): cv.positive_time_period_milliseconds,
            # SunSpec strings are packed into fixed register counts — enforce max lengths
            cv.Required(CONF_MANUFACTURER): cv.All(ascii_string, cv.Length(max=32)),
            cv.Required(CONF_MODEL): cv.All(ascii_string, cv.Length(max=32)),
            cv.Optional(CONF_SERIAL_NUMBER, default=""): cv.All(
                ascii_string, cv.Length(max=32)
            ),
            cv.Optional(CONF_VERSION, default="1.0"): cv.All(
                ascii_string, cv.Length(max=16)
            ),
            cv.Required(CONF_RATED_POWER): cv.int_range(min=1, max=MAX_RATED_POWER),
            cv.Optional(CONF_PHASES, default=1): cv.one_of(1, 3, int=True),
            cv.Required(CONF_AC_POWER): cv.use_id(sensor.Sensor),
            # For phases: 3, ac_voltage is the phase A voltage
            cv.Required(CONF_AC_VOLTAGE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_CURRENT): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_CURRENT_PHASE_A): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_CURRENT_PHASE_B): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_CURRENT_PHASE_C): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_VOLTAGE_PHASE_B): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AC_VOLTAGE_PHASE_C): cv.use_id(sensor.Sensor),
            cv.Required(CONF_AC_FREQUENCY): cv.use_id(sensor.Sensor),
            cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_ENERGY_TOTAL): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DC_POWER): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DC_VOLTAGE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DC_CURRENT): cv.use_id(sensor.Sensor),
            cv.Inclusive(CONF_MODBUS_CONTROLLER_ID, "modbus_write_back"): cv.use_id(
                modbus_controller.ModbusController
            ),
            cv.Inclusive(
                CONF_POWER_LIMIT_REGISTER, "modbus_write_back"
            ): cv.hex_uint16_t,
            cv.Optional(CONF_POWER_LIMIT_NUMBER_ID): cv.use_id(number.Number),
            cv.Optional(CONF_ON_POWER_LIMIT): automation.validate_automation({}),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_at_most_one_key(CONF_POWER_LIMIT_NUMBER_ID, CONF_MODBUS_CONTROLLER_ID),
    _validate_phases,
    cv.require_esphome_version(2026, 4, 0),
)


def _final_validate(config: ConfigType) -> ConfigType:
    full = fv.full_config.get()
    ports = [conf[CONF_PORT] for conf in full.get(DOMAIN, [])]
    if ports.count(config[CONF_PORT]) > 1:
        raise cv.Invalid(
            f"Multiple sunspec servers are configured on port {config[CONF_PORT]}; "
            "each server needs a unique port"
        )
    # grid_meter (same repository) also runs a Modbus TCP server, by default on port 502
    if (grid_meter := full.get("grid_meter")) is not None and grid_meter.get(
        CONF_PORT
    ) == config[CONF_PORT]:
        raise cv.Invalid(
            f"sunspec and grid_meter are both configured on port {config[CONF_PORT]}; "
            "give one of them a different port"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    rated_power = config[CONF_RATED_POWER]
    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_MANUFACTURER],
        config[CONF_MODEL],
        rated_power,
        power_scale_factor(rated_power),
        await cg.get_variable(config[CONF_AC_POWER]),
        await cg.get_variable(config[CONF_AC_VOLTAGE]),
        await cg.get_variable(config[CONF_AC_FREQUENCY]),
        await cg.get_variable(config[CONF_TEMPERATURE]),
    )
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_connections(config[CONF_MAX_CONNECTIONS]))
    cg.add(var.set_unit_address(config[CONF_ADDRESS]))
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT].total_milliseconds))
    if config[CONF_PHASES] == 3:
        cg.add(var.set_model_id(103))
    cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))
    cg.add(var.set_version(config[CONF_VERSION]))

    for conf_key, setter in (
        (CONF_AC_CURRENT, var.set_ac_current),
        (CONF_AC_CURRENT_PHASE_A, var.set_ac_current_phase_a),
        (CONF_AC_CURRENT_PHASE_B, var.set_ac_current_phase_b),
        (CONF_AC_CURRENT_PHASE_C, var.set_ac_current_phase_c),
        (CONF_AC_VOLTAGE_PHASE_B, var.set_ac_voltage_phase_b),
        (CONF_AC_VOLTAGE_PHASE_C, var.set_ac_voltage_phase_c),
        (CONF_ENERGY_TOTAL, var.set_energy_total),
        (CONF_DC_POWER, var.set_dc_power),
        (CONF_DC_VOLTAGE, var.set_dc_voltage),
        (CONF_DC_CURRENT, var.set_dc_current),
    ):
        if (sensor_id := config.get(conf_key)) is not None:
            cg.add(setter(await cg.get_variable(sensor_id)))

    # Write-back code is compiled in only when configured, so configs without
    # modbus_controller / number don't need those components
    if (ctrl_id := config.get(CONF_MODBUS_CONTROLLER_ID)) is not None:
        cg.add_define("USE_SUNSPEC_MODBUS_CONTROLLER")
        ctrl = await cg.get_variable(ctrl_id)
        cg.add(var.set_modbus_controller(ctrl, config[CONF_POWER_LIMIT_REGISTER]))

    if (number_id := config.get(CONF_POWER_LIMIT_NUMBER_ID)) is not None:
        cg.add_define("USE_SUNSPEC_POWER_LIMIT_NUMBER")
        cg.add(var.set_power_limit_number(await cg.get_variable(number_id)))

    for conf in config.get(CONF_ON_POWER_LIMIT, []):
        await automation.build_callback_automation(
            var,
            "add_on_power_limit_callback",
            [(cg.float_, "level"), (cg.bool_, "enabled")],
            conf,
        )
