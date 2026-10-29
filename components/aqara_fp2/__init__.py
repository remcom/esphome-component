from esphome import pins
import esphome.codegen as cg
from esphome.components import i2c, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESET_PIN, CONF_SENSITIVITY
from esphome.core import CORE
from esphome.types import ConfigType

CODEOWNERS = ["@remcom"]
DEPENDENCIES = ["uart"]
DOMAIN = "aqara_fp2"

aqara_fp2_ns = cg.esphome_ns.namespace("aqara_fp2")
FP2Component = aqara_fp2_ns.class_("FP2Component", cg.Component, uart.UARTDevice)
FP2Accel = aqara_fp2_ns.class_("FP2Accel", cg.Component, i2c.I2CDevice)
LocationReportSwitch = aqara_fp2_ns.class_("LocationReportSwitch")

CONF_AQARA_FP2_ID = "aqara_fp2_id"
CONF_AQARA_FP2_ACCEL_ID = "aqara_fp2_accel_id"

CONF_ACCEL = "accel"
CONF_MOUNTING_POSITION = "mounting_position"
CONF_LEFT_RIGHT_REVERSE = "left_right_reverse"
CONF_PRESENCE_SENSITIVITY = "presence_sensitivity"
CONF_INTERFERENCE_GRID = "interference_grid"
CONF_EXIT_GRID = "exit_grid"
CONF_EDGE_GRID = "edge_grid"
CONF_ZONES = "zones"
CONF_GRID = "grid"
CONF_PRESENCE = "presence"
CONF_RADAR_TIMEOUT = "radar_timeout"

# The stock FP2 firmware talks to the radar at 890000 baud, 8N1.
RADAR_BAUD_RATE = 890000

# Address of the accelerometer on the FP2's internal I2C bus.
ACCEL_DEFAULT_ADDRESS = 0x27

MOUNTING_POSITIONS = {
    "wall": 0x01,
    "left_corner": 0x02,
    "right_corner": 0x03,
}

SENSITIVITY_LEVELS = {
    "low": 1,
    "medium": 2,
    "high": 3,
}

# Zone ids are indexes into the radar's 32-entry zone activation list (MAX_ZONES in aqara_fp2.h).
MAX_ZONES = 30

# Active detection grid geometry. Users describe a 14x14 area which occupies rows 0-13 and
# columns 2-15 of the radar's native 20-row x 16-column (320-bit / 40-byte) map.
GRID_INPUT_ROWS = 14
GRID_INPUT_COLS = 14
GRID_COL_OFFSET = 2
GRID_SIZE = 40


def parse_ascii_grid(value: object) -> list[int]:
    """Parse a 14x14 ASCII grid into the radar's 40-byte map.

    'x'/'X' mark an active cell; '.'/'-' mark an inactive cell. Spaces are ignored.
    """
    value = cv.string(value)
    lines = [line.strip() for line in value.strip().splitlines() if line.strip()]
    if len(lines) != GRID_INPUT_ROWS:
        raise cv.Invalid(
            f"Grid must have exactly {GRID_INPUT_ROWS} rows, got {len(lines)}"
        )

    grid = bytearray(GRID_SIZE)
    for row, line in enumerate(lines):
        cells = line.replace(" ", "")
        if len(cells) != GRID_INPUT_COLS:
            raise cv.Invalid(
                f"Row {row + 1} must have {GRID_INPUT_COLS} cells, got {len(cells)}: '{cells}'"
            )
        row_bits = 0
        for col, char in enumerate(cells):
            if char in ("x", "X"):
                # Bit 15 is column 0; the active area starts GRID_COL_OFFSET columns in.
                row_bits |= 1 << (15 - (col + GRID_COL_OFFSET))
            elif char not in (".", "-"):
                raise cv.Invalid(f"Invalid grid character '{char}' in row {row + 1}")
        grid[row * 2] = (row_bits >> 8) & 0xFF
        grid[row * 2 + 1] = row_bits & 0xFF
    return list(grid)


def require_accel_for(*keys: str):
    """Platform validator: resolve the accelerometer ID when any of `keys` is configured.

    Fails with a missing-ID error when an accelerometer entity is configured without an
    `accel:` block, while radar-only platform entries need no accelerometer at all.
    """
    accel_id_schema = cv.Schema(
        {cv.GenerateID(CONF_AQARA_FP2_ACCEL_ID): cv.use_id(FP2Accel)},
        extra=cv.ALLOW_EXTRA,
    )

    def validator(config: ConfigType) -> ConfigType:
        if any(key in config for key in keys):
            return accel_id_schema(config)
        return config

    return validator


ZONE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SENSITIVITY, default="medium"): cv.enum(SENSITIVITY_LEVELS),
        cv.Required(CONF_GRID): parse_ascii_grid,
    }
)

ACCEL_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FP2Accel),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(ACCEL_DEFAULT_ADDRESS))
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FP2Component),
            cv.Optional(CONF_ACCEL): ACCEL_SCHEMA,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_MOUNTING_POSITION, default="wall"): cv.enum(
                MOUNTING_POSITIONS
            ),
            cv.Optional(CONF_LEFT_RIGHT_REVERSE, default=False): cv.boolean,
            cv.Optional(CONF_PRESENCE_SENSITIVITY, default="medium"): cv.enum(
                SENSITIVITY_LEVELS
            ),
            cv.Optional(CONF_INTERFERENCE_GRID): parse_ascii_grid,
            cv.Optional(CONF_EXIT_GRID): parse_ascii_grid,
            cv.Optional(CONF_EDGE_GRID): parse_ascii_grid,
            cv.Optional(
                CONF_RADAR_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ZONES): cv.All(
                cv.ensure_list(ZONE_SCHEMA), cv.Length(max=MAX_ZONES)
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.require_esphome_version(2026, 4, 0),
)


def _final_validate(config: ConfigType) -> ConfigType:
    # The host UART only supports standard baud rates, so host builds (used for clang-tidy) are
    # exempt from the radar's baud rate.
    return uart.final_validate_device_schema(
        DOMAIN,
        baud_rate=None if CORE.is_host else RADAR_BAUD_RATE,
        require_tx=True,
        require_rx=True,
        parity="NONE",
        stop_bits=1,
    )(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    # Zones are created before the first await, so the binary_sensor platform (which waits for this
    # variable) always finds them when it attaches zone entities by index.
    if zones := config.get(CONF_ZONES):
        cg.add(var.init_zones(len(zones)))
        for zone in zones:
            cg.add(var.add_zone(zone[CONF_GRID], zone[CONF_SENSITIVITY]))

    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if (accel_config := config.get(CONF_ACCEL)) is not None:
        cg.add_define("USE_AQARA_FP2_ACCEL")
        accel = cg.new_Pvariable(accel_config[CONF_ID])
        await cg.register_component(accel, accel_config)
        await i2c.register_i2c_device(accel, accel_config)
        cg.add(var.set_accel(accel))
    if (reset_pin := config.get(CONF_RESET_PIN)) is not None:
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(reset_pin)))

    cg.add(var.set_mounting_position(config[CONF_MOUNTING_POSITION]))
    cg.add(var.set_left_right_reverse(config[CONF_LEFT_RIGHT_REVERSE]))
    cg.add(var.set_presence_sensitivity(config[CONF_PRESENCE_SENSITIVITY]))
    cg.add(var.set_radar_timeout(config[CONF_RADAR_TIMEOUT].total_milliseconds))

    if (grid := config.get(CONF_INTERFERENCE_GRID)) is not None:
        cg.add(var.set_interference_grid(grid))
    if (grid := config.get(CONF_EXIT_GRID)) is not None:
        cg.add(var.set_exit_grid(grid))
    if (grid := config.get(CONF_EDGE_GRID)) is not None:
        cg.add(var.set_edge_grid(grid))
