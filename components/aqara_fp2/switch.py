import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG
from esphome.types import ConfigType

from . import CONF_AQARA_FP2_ID, FP2Component, LocationReportSwitch

DEPENDENCIES = ["aqara_fp2"]

CONF_LOCATION_REPORT = "location_report"

ICON_CROSSHAIRS_GPS = "mdi:crosshairs-gps"

CONFIG_SCHEMA = {
    cv.GenerateID(CONF_ID): cv.declare_id(cg.EntityBase),
    cv.GenerateID(CONF_AQARA_FP2_ID): cv.use_id(FP2Component),
    cv.Optional(CONF_LOCATION_REPORT): switch.switch_schema(
        LocationReportSwitch,
        default_restore_mode="RESTORE_DEFAULT_ON",
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon=ICON_CROSSHAIRS_GPS,
    ),
}


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AQARA_FP2_ID])
    switches = switch.sub_switches(config, parent=hub)
    await switches(CONF_LOCATION_REPORT, hub.set_location_report_switch)
