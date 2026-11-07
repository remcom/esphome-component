import hashlib
from pathlib import Path

from esphome import automation, external_files, pins
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import (
    CONF_ON_ERROR,
    CONF_PATH,
    CONF_RAW_DATA_ID,
    CONF_RESET_PIN,
    CONF_URL,
    CONF_VERSION,
)
from esphome.core import CORE, HexInt
from esphome.types import ConfigType

CODEOWNERS = ["@remcom"]
DEPENDENCIES = ["i2c"]
DOMAIN = "respeaker_lite"

CONF_AUTO_UPDATE_MIN_VERSION = "auto_update_min_version"
CONF_FIRMWARE = "firmware"
CONF_MD5 = "md5"
CONF_ON_BEGIN = "on_begin"
CONF_ON_END = "on_end"
CONF_ON_PROGRESS = "on_progress"
CONF_RESPEAKER_LITE_ID = "respeaker_lite_id"

I2C_ADDRESS = 0x42
# DFU of XMOS firmware older than this can erase its own factory image
# (formatBCE/Respeaker-Lite-ESPHome-integration issue #94)
DEFAULT_AUTO_UPDATE_MIN_VERSION = "1.0.8"

respeaker_lite_ns = cg.esphome_ns.namespace("respeaker_lite")
RespeakerLite = respeaker_lite_ns.class_("RespeakerLite", cg.Component, i2c.I2CDevice)


# `x` as the argument name keeps formatBCE's YAML working
DFU_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_BEGIN, "add_on_dfu_begin_callback"),
    automation.CallbackAutomation(
        CONF_ON_PROGRESS, "add_on_dfu_progress_callback", [(cg.float_, "x")]
    ),
    automation.CallbackAutomation(CONF_ON_END, "add_on_dfu_end_callback"),
    automation.CallbackAutomation(
        CONF_ON_ERROR, "add_on_dfu_error_callback", [(cg.uint8, "x")]
    ),
)


def _download_path(url: str) -> Path:
    key = hashlib.sha256(url.encode()).hexdigest()[:8]
    return external_files.compute_local_file_dir(DOMAIN) / key


def firmware_path(config: ConfigType) -> Path:
    """Local file holding the firmware of a validated `firmware:` block."""
    if (path := config.get(CONF_PATH)) is not None:
        return Path(CORE.relative_config_path(path))
    return _download_path(config[CONF_URL])


def version_tuple(version: str) -> tuple[int, int, int]:
    """'1.1.0' -> (1, 1, 0); every part must fit the XMOS's uint8 version fields."""
    parts = tuple(int(p) for p in str(version).split("."))
    if len(parts) != 3 or any(p > 255 for p in parts):
        raise cv.Invalid(
            f"Version must be major.minor.patch with parts <= 255: {version}"
        )
    return parts


def _validate_version(value: str) -> str:
    value = cv.version_number(value)
    version_tuple(value)
    return value


def _validate_md5(value: str) -> str:
    value = cv.string_strict(value).lower()
    if len(value) != 32 or any(c not in "0123456789abcdef" for c in value):
        raise cv.Invalid("md5 must be 32 hexadecimal characters")
    return value


def _load_firmware(config: ConfigType) -> ConfigType:
    """Download the firmware (`url`) and check it against `md5`."""
    if (url := config.get(CONF_URL)) is not None:
        external_files.download_content(url, _download_path(url))
    path = firmware_path(config)
    try:
        firmware = path.read_bytes()
    except OSError as e:
        raise cv.Invalid(f"Could not read firmware file {path}: {e}") from e
    if not firmware:
        raise cv.Invalid(f"Firmware file {path} is empty")
    if hashlib.md5(firmware).hexdigest() != config[CONF_MD5]:
        raise cv.Invalid(f"{CONF_MD5} does not match the contents of {path}")
    return config


FIRMWARE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Exclusive(CONF_URL, "source"): cv.url,
            cv.Exclusive(CONF_PATH, "source"): cv.file_,
            cv.Required(CONF_VERSION): _validate_version,
            cv.Required(CONF_MD5): _validate_md5,
            cv.Optional(
                CONF_AUTO_UPDATE_MIN_VERSION, default=DEFAULT_AUTO_UPDATE_MIN_VERSION
            ): _validate_version,
            cv.Optional(CONF_ON_BEGIN): automation.validate_automation({}),
            cv.Optional(CONF_ON_PROGRESS): automation.validate_automation({}),
            cv.Optional(CONF_ON_END): automation.validate_automation({}),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation({}),
        }
    ),
    cv.has_exactly_one_key(CONF_URL, CONF_PATH),
    _load_firmware,
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RespeakerLite),
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_FIRMWARE): FIRMWARE_SCHEMA,
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(I2C_ADDRESS)),
    cv.require_esphome_version(2026, 11, 0),
)


async def to_code(config: ConfigType) -> None:
    reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    var = await i2c.new_i2c_component(config, reset_pin)

    if (fw := config.get(CONF_FIRMWARE)) is None:
        return

    firmware = firmware_path(fw).read_bytes()
    data = cg.progmem_array(config[CONF_RAW_DATA_ID], [HexInt(b) for b in firmware])
    cg.add(var.set_firmware(data, len(firmware), *version_tuple(fw[CONF_VERSION])))
    cg.add(
        var.set_auto_update_min_version(
            *version_tuple(fw[CONF_AUTO_UPDATE_MIN_VERSION])
        )
    )

    await automation.build_callback_automations(var, fw, DFU_AUTOMATIONS)


RESPEAKER_LITE_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(RespeakerLite)}
)

# No C++ action classes: each action is one call on the component
for _name, _call in (
    ("flash", "start_dfu_update()"),
    ("mute_speaker", "mute_speaker()"),
    ("unmute_speaker", "unmute_speaker()"),
):
    automation.register_apply_action(
        f"respeaker_lite.{_name}",
        RESPEAKER_LITE_ACTION_SCHEMA,
        automation.ApplyCall(_call),
    )
