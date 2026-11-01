"""Schema validation tests — run without hardware, requires esphome installed."""

from collections.abc import Callable
import json
from types import ModuleType

import pytest

import esphome.config_validation as cv
from esphome.types import ConfigType

ALBUM_ID = "9c5e2bd7-3b0f-4bb1-90b4-4a2e0e6a7b3d"

REQUIRED = {
    "url": "http://immich.local:2283",
    "api_key": "TestApiKey123",
    "album_id": ALBUM_ID,
    "image_id": "slideshow_image",
}


@pytest.fixture
def immich(load_component: Callable[[str], ModuleType]) -> ModuleType:
    return load_component("immich")


@pytest.fixture
def schema(immich: ModuleType) -> Callable[[ConfigType], ConfigType]:
    return immich.CONFIG_SCHEMA


def test_valid_config_accepted(schema: Callable[[ConfigType], ConfigType]) -> None:
    result = schema(REQUIRED)
    assert str(result["album_id"]) == ALBUM_ID
    assert result["update_interval"].total_milliseconds == 30000


@pytest.mark.parametrize("key", REQUIRED)
def test_required_keys(schema: Callable[[ConfigType], ConfigType], key: str) -> None:
    partial = {k: v for k, v in REQUIRED.items() if k != key}
    with pytest.raises(cv.Invalid, match=key):
        schema(partial)


def test_album_id_must_be_uuid(schema: Callable[[ConfigType], ConfigType]) -> None:
    with pytest.raises(cv.Invalid):
        schema({**REQUIRED, "album_id": "not-a-uuid"})


def test_url_must_be_valid(schema: Callable[[ConfigType], ConfigType]) -> None:
    with pytest.raises(cv.Invalid):
        schema({**REQUIRED, "url": "immich.local"})


def test_update_interval_configurable(
    schema: Callable[[ConfigType], ConfigType],
) -> None:
    result = schema({**REQUIRED, "update_interval": "2min"})
    assert result["update_interval"].total_milliseconds == 120000


def test_search_body(immich: ModuleType) -> None:
    """One random image from the album, without EXIF and people, as compact JSON."""
    body = immich.search_body(ALBUM_ID)
    assert " " not in body
    assert json.loads(body) == {
        "albumIds": [ALBUM_ID],
        "size": 1,
        "type": "IMAGE",
        "withExif": False,
        "withPeople": False,
    }
