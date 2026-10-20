import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.components import sensor, modbus_controller, number
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PORT, CONF_TRIGGER_ID, CONF_VERSION

DEPENDENCIES = ["network", "sensor"]
MULTI_CONF = True

sunspec_ns = cg.esphome_ns.namespace("sunspec")
SunspecComponent = sunspec_ns.class_("SunspecComponent", cg.Component)
PowerLimitTrigger = sunspec_ns.class_(
    "PowerLimitTrigger", automation.Trigger.template(cg.float_, cg.bool_)
)

CONF_MANUFACTURER = "manufacturer"
CONF_MODEL_NAME = "model"
CONF_SERIAL_NUMBER = "serial_number"
CONF_RATED_POWER = "rated_power"
CONF_MAX_CONNECTIONS = "max_connections"
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
CONF_TEMPERATURE = "temperature"
CONF_ENERGY_TOTAL = "energy_total"
CONF_DC_POWER = "dc_power"
CONF_DC_VOLTAGE = "dc_voltage"
CONF_DC_CURRENT = "dc_current"
CONF_MODBUS_CONTROLLER_ID = "modbus_controller_id"
CONF_POWER_LIMIT_REGISTER = "power_limit_register"
CONF_POWER_LIMIT_NUMBER_ID = "power_limit_number_id"
CONF_ON_POWER_LIMIT = "on_power_limit"


PHASE_SENSOR_KEYS = [
    CONF_AC_CURRENT_PHASE_A,
    CONF_AC_CURRENT_PHASE_B,
    CONF_AC_CURRENT_PHASE_C,
    CONF_AC_VOLTAGE_PHASE_B,
    CONF_AC_VOLTAGE_PHASE_C,
]


def _validate_phases(config):
    if config[CONF_PHASES] == 1:
        for key in PHASE_SENSOR_KEYS:
            if key in config:
                raise cv.Invalid(f"'{key}' requires 'phases: 3'")
    return config


def _validate_power_limit(config):
    has_register = CONF_POWER_LIMIT_REGISTER in config
    has_controller = CONF_MODBUS_CONTROLLER_ID in config
    if has_register and not has_controller:
        raise cv.Invalid(
            f"'{CONF_MODBUS_CONTROLLER_ID}' is required when '{CONF_POWER_LIMIT_REGISTER}' is set"
        )
    if has_controller and not has_register:
        raise cv.Invalid(
            f"'{CONF_POWER_LIMIT_REGISTER}' is required when '{CONF_MODBUS_CONTROLLER_ID}' is set"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
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
            cv.Required(CONF_MANUFACTURER): cv.All(cv.string, cv.Length(max=32)),
            cv.Required(CONF_MODEL_NAME): cv.All(cv.string, cv.Length(max=32)),
            cv.Optional(CONF_SERIAL_NUMBER, default=""): cv.All(
                cv.string, cv.Length(max=32)
            ),
            cv.Optional(CONF_VERSION, default="1.0"): cv.All(
                cv.string, cv.Length(max=16)
            ),
            cv.Required(CONF_RATED_POWER): cv.All(cv.positive_int, cv.Range(max=32767)),
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
            cv.Optional(CONF_MODBUS_CONTROLLER_ID): cv.use_id(
                modbus_controller.ModbusController
            ),
            cv.Optional(CONF_POWER_LIMIT_REGISTER): cv.positive_int,
            cv.Optional(CONF_POWER_LIMIT_NUMBER_ID): cv.use_id(number.Number),
            cv.Optional(CONF_ON_POWER_LIMIT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PowerLimitTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_phases,
    _validate_power_limit,
)


def _final_validate(config):
    full = fv.full_config.get()
    ports = [conf[CONF_PORT] for conf in full.get("sunspec", [])]
    if ports.count(config[CONF_PORT]) > 1:
        raise cv.Invalid(
            f"Multiple sunspec servers are configured on port {config[CONF_PORT]}; "
            "each server needs a unique port"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_connections(config[CONF_MAX_CONNECTIONS]))
    cg.add(var.set_unit_address(config[CONF_ADDRESS]))
    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT].total_milliseconds))
    if config[CONF_PHASES] == 3:
        cg.add(var.set_model_id(103))

    cg.add(var.set_manufacturer(config[CONF_MANUFACTURER]))
    cg.add(var.set_model(config[CONF_MODEL_NAME]))
    cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))
    cg.add(var.set_version(config[CONF_VERSION]))
    cg.add(var.set_rated_power(config[CONF_RATED_POWER]))

    for conf_key, setter in [
        (CONF_AC_POWER, "set_ac_power"),
        (CONF_AC_VOLTAGE, "set_ac_voltage"),
        (CONF_AC_FREQUENCY, "set_ac_frequency"),
        (CONF_TEMPERATURE, "set_temperature"),
    ]:
        sens = await cg.get_variable(config[conf_key])
        cg.add(getattr(var, setter)(sens))

    for conf_key, setter in [
        (CONF_AC_CURRENT, "set_ac_current"),
        (CONF_AC_CURRENT_PHASE_A, "set_ac_current_phase_a"),
        (CONF_AC_CURRENT_PHASE_B, "set_ac_current_phase_b"),
        (CONF_AC_CURRENT_PHASE_C, "set_ac_current_phase_c"),
        (CONF_AC_VOLTAGE_PHASE_B, "set_ac_voltage_phase_b"),
        (CONF_AC_VOLTAGE_PHASE_C, "set_ac_voltage_phase_c"),
        (CONF_ENERGY_TOTAL, "set_energy_total"),
        (CONF_DC_POWER, "set_dc_power"),
        (CONF_DC_VOLTAGE, "set_dc_voltage"),
        (CONF_DC_CURRENT, "set_dc_current"),
    ]:
        if conf_key in config:
            sens = await cg.get_variable(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    if CONF_MODBUS_CONTROLLER_ID in config:
        ctrl = await cg.get_variable(config[CONF_MODBUS_CONTROLLER_ID])
        cg.add(var.set_modbus_controller(ctrl))

    if CONF_POWER_LIMIT_REGISTER in config:
        cg.add(var.set_power_limit_register(config[CONF_POWER_LIMIT_REGISTER]))

    if CONF_POWER_LIMIT_NUMBER_ID in config:
        num = await cg.get_variable(config[CONF_POWER_LIMIT_NUMBER_ID])
        cg.add(var.set_power_limit_number(num))

    for conf in config.get(CONF_ON_POWER_LIMIT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.float_, "level"), (cg.bool_, "enabled")], conf
        )
