"""
Integration tests for behaviour that needs changing sensor inputs: the stale-data watchdog,
hold-on-NaN shadow registers, the energy clamp and per-phase NaN handling.

Requires the host build of tests/components/grid_meter/test-dynamic.host.yaml (port 5022,
data_timeout 5 s), started with ESPHOME_TEST_CTL=<file>:

    pytest tests/grid_meter/test_dynamic.py --device-ip=127.0.0.1 --device-port=5022 \
        --ctl-file=<file> -v
"""

import struct
import time

from pymodbus.client import ModbusTcpClient
from pymodbus.exceptions import ModbusException
import pytest

UNIT = 1
DATA_TIMEOUT_S = 5
SENSOR_PERIOD_S = 0.5


@pytest.fixture
def addr(request):
    return request.config.getoption("--device-ip"), request.config.getoption(
        "--device-port"
    )


def read_regs(addr, start: int, count: int) -> list[int] | None:
    """Read on a fresh connection; None when the server refuses or drops the request."""
    c = ModbusTcpClient(addr[0], port=addr[1], timeout=1, retries=0)
    try:
        if not c.connect():
            return None
        rr = c.read_holding_registers(start, count=count, device_id=UNIT)
        return None if rr.isError() else rr.registers
    except ModbusException:
        return None
    finally:
        c.close()


def int32(regs: list[int], idx: int = 0) -> int:
    """Reg_s32l: low word first."""
    return struct.unpack(">i", struct.pack(">I", (regs[idx + 1] << 16) | regs[idx]))[0]


def wait_for(predicate, timeout: float, message: str):
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() > deadline:
            pytest.fail(message)
        time.sleep(0.1)


def wait_for_service(addr):
    wait_for(
        lambda: read_regs(addr, 0, 1) is not None,
        DATA_TIMEOUT_S + 5,
        "Modbus service did not (re)start",
    )


def test_defaults_served(addr, ctl):
    wait_for_service(addr)
    regs = read_regs(addr, 0x0000, 0x50)
    assert int32(regs, 0x00) == 2301
    assert int32(regs, 0x12) == 12340


def test_watchdog_suspends_and_resumes(addr, ctl):
    """No valid power reading on any phase for data_timeout -> connections refused; resumes with data."""
    wait_for_service(addr)
    ctl.set(power_import="nan", power_import_l2="nan")
    time.sleep(DATA_TIMEOUT_S - 2)
    assert read_regs(addr, 0, 1) is not None, "suspended before data_timeout elapsed"
    wait_for(
        lambda: read_regs(addr, 0, 1) is None,
        DATA_TIMEOUT_S + 3,
        "service not suspended after data_timeout",
    )

    ctl.set(power_import=500.0)
    wait_for(
        lambda: read_regs(addr, 0x12, 2) is not None,
        SENSOR_PERIOD_S + 3,
        "service did not resume when power data returned",
    )
    wait_for(
        lambda: int32(read_regs(addr, 0x12, 2)) == 5000,
        3,
        "power register not updated after resume",
    )


def test_watchdog_closes_existing_clients(addr, ctl):
    """A client that is already connected is dropped when the watchdog trips."""
    wait_for_service(addr)
    c = ModbusTcpClient(addr[0], port=addr[1], timeout=1, retries=0)
    assert c.connect()
    try:
        assert not c.read_holding_registers(0, count=1, device_id=UNIT).isError()
        ctl.set(power_import="nan", power_import_l2="nan")

        def dropped() -> bool:
            try:
                return c.read_holding_registers(0, count=1, device_id=UNIT).isError()
            except ModbusException:
                return True

        wait_for(dropped, DATA_TIMEOUT_S + 3, "existing client kept after suspend")
    finally:
        c.close()


def test_power_nan_reads_zero(addr, ctl):
    """Instantaneous power is zeroed (not held) when a sensor is NaN."""
    wait_for_service(addr)
    ctl.set(power_import="nan")
    wait_for(
        lambda: (r := read_regs(addr, 0x12, 2)) is not None and int32(r) == 0,
        SENSOR_PERIOD_S + 2,
        "power not zeroed on NaN",
    )


def test_voltage_and_current_hold_on_nan(addr, ctl):
    wait_for_service(addr)
    ctl.set(voltage=231.0, current=2.5)
    wait_for(
        lambda: (
            int32(read_regs(addr, 0x00, 2)) == 2310
            and int32(read_regs(addr, 0x0C, 2)) == 2500
        ),
        SENSOR_PERIOD_S + 2,
        "new voltage/current not served",
    )
    ctl.set(voltage="nan", current="nan")
    time.sleep(2 * SENSOR_PERIOD_S + 0.5)  # several NaN publishes
    assert int32(read_regs(addr, 0x00, 2)) == 2310
    assert int32(read_regs(addr, 0x0C, 2)) == 2500
    ctl.set(voltage=229.5)
    wait_for(
        lambda: int32(read_regs(addr, 0x00, 2)) == 2295,
        SENSOR_PERIOD_S + 2,
        "voltage not updated after NaN",
    )


def test_current_is_magnitude(addr, ctl):
    wait_for_service(addr)
    ctl.set(current=-3.0)
    wait_for(
        lambda: int32(read_regs(addr, 0x0C, 2)) == 3000,
        SENSOR_PERIOD_S + 2,
        "negative current not stored as magnitude",
    )


def test_energy_holds_on_nan_of_either_tariff(addr, ctl):
    """Energy import = (t1 + t2) * 10; a NaN tariff keeps the last total."""
    wait_for_service(addr)
    assert (
        int32(read_regs(addr, 0x34, 2)) == 30007
    )  # (1000.5 + 2000.25) kWh = 30007.5, truncated
    ctl.set(energy_import_t1="nan", energy_import_t2=5000.0)
    time.sleep(2 * SENSOR_PERIOD_S + 0.5)
    assert int32(read_regs(addr, 0x34, 2)) == 30007
    ctl.set(energy_import_t1=1000.0)
    wait_for(
        lambda: int32(read_regs(addr, 0x34, 2)) == 60000,
        SENSOR_PERIOD_S + 2,
        "energy not updated after NaN",
    )
    # The L1 mirror follows
    assert read_regs(addr, 0x40, 2) == read_regs(addr, 0x34, 2)


def test_energy_clamped_at_int32_max(addr, ctl):
    wait_for_service(addr)
    ctl.set(energy_export_t1=3e8, energy_export_t2=3e8)
    wait_for(
        lambda: read_regs(addr, 0x4E, 2) == [0xFFFF, 0x7FFF],
        SENSOR_PERIOD_S + 2,
        "export energy not clamped to INT32_MAX",
    )
    assert read_regs(addr, 0x46, 2) == [0xFFFF, 0x7FFF]


def test_l2_power_nan_zeroes_only_l2(addr, ctl):
    """A NaN L2 sensor zeroes L2 power, holds L2 voltage; L1 and the watchdog are unaffected."""
    wait_for_service(addr)
    wait_for(
        lambda: int32(read_regs(addr, 0x14, 2)) == 1000,
        SENSOR_PERIOD_S + 2,
        "L2 power not served",
    )
    assert int32(read_regs(addr, 0x28, 2)) == 12340 + 1000  # total = L1 + L2
    assert int32(read_regs(addr, 0x02, 2)) == 2312

    ctl.set(power_import_l2="nan", voltage_l2="nan")
    wait_for(
        lambda: int32(read_regs(addr, 0x14, 2)) == 0,
        SENSOR_PERIOD_S + 2,
        "L2 power not zeroed on NaN",
    )
    assert int32(read_regs(addr, 0x12, 2)) == 12340
    assert int32(read_regs(addr, 0x28, 2)) == 12340
    assert int32(read_regs(addr, 0x02, 2)) == 2312, "L2 voltage not held"

    # L1 keeps the watchdog satisfied well past data_timeout
    time.sleep(DATA_TIMEOUT_S + 1)
    assert read_regs(addr, 0, 1) is not None
