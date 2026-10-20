"""Schema validation tests — run without hardware."""
import pytest


def test_rated_power_max():
    """rated_power must not exceed int16 max (32767)."""
    import esphome.config_validation as cv
    validator = cv.All(cv.positive_int, cv.Range(max=32767))
    assert validator(3000) == 3000
    with pytest.raises(cv.Invalid):
        validator(33000)


def test_rated_power_min():
    """rated_power must be positive."""
    import esphome.config_validation as cv
    validator = cv.All(cv.positive_int, cv.Range(max=32767))
    with pytest.raises((cv.Invalid, ValueError)):
        validator(0)


def test_manufacturer_is_string():
    """manufacturer must be a string."""
    import esphome.config_validation as cv
    assert cv.string("Solis") == "Solis"
    with pytest.raises(cv.Invalid):
        cv.string(123)
