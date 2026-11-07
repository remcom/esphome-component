"""Schema validation tests — run without hardware, requires esphome installed."""

from collections.abc import Callable
import hashlib
from pathlib import Path
from types import ModuleType

import pytest

import esphome.config_validation as cv
from esphome.core import CORE
from esphome.types import ConfigType

FIRMWARE = bytes(range(256)) * 3


@pytest.fixture
def respeaker_lite(load_component: Callable[[str], ModuleType]) -> ModuleType:
    return load_component("respeaker_lite")


@pytest.fixture
def firmware_file(tmp_path: Path) -> Path:
    CORE.config_path = tmp_path / "test.yaml"
    path = tmp_path / "xmos.bin"
    path.write_bytes(FIRMWARE)
    return path


@pytest.fixture
def firmware_schema(respeaker_lite: ModuleType) -> Callable[[ConfigType], ConfigType]:
    return respeaker_lite.FIRMWARE_SCHEMA


def firmware_config(path: Path, **overrides: str) -> ConfigType:
    return {
        "path": str(path),
        "version": "1.1.0",
        "md5": hashlib.md5(FIRMWARE).hexdigest(),
        **overrides,
    }


def test_firmware_from_path_accepted(
    firmware_schema: Callable[[ConfigType], ConfigType],
    respeaker_lite: ModuleType,
    firmware_file: Path,
) -> None:
    result = firmware_schema(firmware_config(firmware_file))
    assert result["auto_update_min_version"] == "1.0.8"
    assert respeaker_lite.firmware_path(result).read_bytes() == FIRMWARE


def test_md5_is_case_insensitive(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path
) -> None:
    md5 = hashlib.md5(FIRMWARE).hexdigest().upper()
    result = firmware_schema(firmware_config(firmware_file, md5=md5))
    assert result["md5"] == md5.lower()


def test_md5_mismatch_rejected(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path
) -> None:
    with pytest.raises(cv.Invalid, match="md5 does not match"):
        firmware_schema(firmware_config(firmware_file, md5="0" * 32))


@pytest.mark.parametrize("md5", ["abc", "z" * 32, "0" * 33])
def test_md5_format_rejected(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path, md5: str
) -> None:
    with pytest.raises(cv.Invalid, match="hexadecimal"):
        firmware_schema(firmware_config(firmware_file, md5=md5))


def test_missing_file_rejected(
    firmware_schema: Callable[[ConfigType], ConfigType], tmp_path: Path
) -> None:
    CORE.config_path = tmp_path / "test.yaml"
    with pytest.raises(cv.Invalid):
        firmware_schema(firmware_config(tmp_path / "missing.bin"))


def test_url_and_path_exclusive(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path
) -> None:
    with pytest.raises(cv.Invalid):
        firmware_schema(
            firmware_config(firmware_file, url="https://example.com/xmos.bin")
        )


def test_url_or_path_required(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path
) -> None:
    config = firmware_config(firmware_file)
    del config["path"]
    with pytest.raises(cv.Invalid):
        firmware_schema(config)


@pytest.mark.parametrize("key", ["version", "auto_update_min_version"])
@pytest.mark.parametrize("version", ["1.256.0", "abc", "1.2.3.4"])
def test_invalid_versions_rejected(
    firmware_schema: Callable[[ConfigType], ConfigType],
    firmware_file: Path,
    key: str,
    version: str,
) -> None:
    with pytest.raises(cv.Invalid):
        firmware_schema(firmware_config(firmware_file, **{key: version}))


def test_version_tuple(respeaker_lite: ModuleType) -> None:
    assert respeaker_lite.version_tuple("1.0.8") == (1, 0, 8)
    assert respeaker_lite.version_tuple("255.255.255") == (255, 255, 255)
    with pytest.raises(cv.Invalid):
        respeaker_lite.version_tuple("1.2.3.4")


def test_short_version_padded(
    firmware_schema: Callable[[ConfigType], ConfigType], firmware_file: Path
) -> None:
    result = firmware_schema(firmware_config(firmware_file, version="1.1"))
    assert result["version"] == "1.1.0"
