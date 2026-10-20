"""
Integration tests for sensor staleness: a sensor that stops publishing is reported as
"not implemented" and the inverter state falls back to Off.

Requires the host build of tests/components/sunspec/test-dynamic.host.yaml (port 5023,
stale_timeout 4 s), started with ESPHOME_TEST_CTL=<file>:

    pytest tests/sunspec/test_dynamic.py --device-ip=127.0.0.1 --device-port=5023 \
        --ctl-file=<file> -v
"""

import time

import pytest

UNIT = 1
STALE_TIMEOUT_S = 4
STALE_SCAN_S = 1  # staleness is evaluated once a second
SENSOR_PERIOD_S = 1

REG_VOLTAGE = 40080
REG_FREQUENCY = 40086
REG_POWER = 40084
REG_STATE = 40108
REG_ENERGY = 40094
REG_DC_POWER = 40101
NI_INT16 = 0x8000
NI_UINT16 = 0xFFFF
ST_OFF, ST_MPPT = 1, 4


def read(client, addr: int, count: int = 1) -> list[int]:
    rr = client.read_holding_registers(addr, count=count, device_id=UNIT)
    assert not rr.isError(), f"read {addr} failed: {rr}"
    return rr.registers


def wait_for(predicate, timeout: float, message: str):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() > deadline:
            pytest.fail(message)
        time.sleep(0.2)


STALE_WAIT_S = STALE_TIMEOUT_S + 2 * STALE_SCAN_S + 1


def test_live_values_served(client, ctl):
    wait_for(lambda: read(client, REG_STATE) == [ST_MPPT], 5, "inverter not producing")
    assert read(client, REG_POWER) == [2500]
    assert read(client, REG_VOLTAGE) == [2314]


def test_stale_power_reports_not_implemented_and_state_off(client, ctl):
    wait_for(lambda: read(client, REG_POWER) == [2500], 5, "power not served")
    ctl.set(ac_power="skip")
    time.sleep(STALE_TIMEOUT_S - 2)
    assert read(client, REG_POWER) == [2500], "went stale before stale_timeout"
    wait_for(
        lambda: read(client, REG_POWER) == [NI_INT16],
        STALE_WAIT_S,
        "stale power not reported as not implemented",
    )
    assert read(client, REG_STATE) == [ST_OFF]
    # Other sensors keep publishing and stay live
    assert read(client, REG_VOLTAGE) == [2314]

    ctl.reset()  # resume publishing
    wait_for(
        lambda: (
            read(client, REG_POWER) == [2500] and read(client, REG_STATE) == [ST_MPPT]
        ),
        SENSOR_PERIOD_S + 3,
        "power/state did not recover when the sensor resumed",
    )


def test_stale_sensors_use_their_own_not_implemented_value(client, ctl):
    """int16 registers report 0x8000, uint16 registers 0xFFFF."""
    wait_for(lambda: read(client, REG_DC_POWER) == [2600], 5, "dc power not served")
    ctl.set(dc_power="skip", ac_frequency="skip")
    wait_for(
        lambda: (
            read(client, REG_DC_POWER) == [NI_INT16]
            and read(client, REG_FREQUENCY) == [NI_UINT16]
        ),
        STALE_WAIT_S,
        "stale sensors not reported as not implemented",
    )
    assert read(client, REG_STATE) == [ST_MPPT], "state must follow AC power only"


def test_energy_accumulator_keeps_last_total(client, ctl):
    """Energy is an accumulator: it keeps its last total when the sensor goes quiet."""
    wh = [12345500 >> 16, 12345500 & 0xFFFF]  # 12345.5 kWh
    wait_for(lambda: read(client, REG_ENERGY, 2) == wh, 5, "energy not served")
    ctl.set(energy_total="skip")
    time.sleep(STALE_WAIT_S)
    assert read(client, REG_ENERGY, 2) == wh
