"""
Integration tests -- require a device running sunspec, reachable on the network (or the
host build of tests/components/sunspec/test-number.host.yaml, see README).

Run: pytest tests/sunspec/test_sunspec.py --device-ip=<IP> [--allow-writes]

Tests that write power-limit registers are skipped unless --allow-writes is given: on a
device with write-back configured they briefly limit the real inverter.
"""

from collections.abc import Callable, Iterator
import time

from pymodbus.client import ModbusTcpClient
import pytest

UNIT = 1
SUNS = [0x5375, 0x6E53]


def read(client: ModbusTcpClient, addr: int, count: int = 1) -> list[int]:
    rr = client.read_holding_registers(addr, count=count, device_id=UNIT)
    assert not rr.isError(), f"read {addr} failed: {rr}"
    return rr.registers


@pytest.fixture
def restore_limit(client: ModbusTcpClient, allow_writes: None) -> Iterator[None]:
    """Leave the power limit disabled at 100 % with no revert timer after a write test."""
    yield
    client.write_registers(40155, [10000, 0xFFFF, 0xFFFF, 0xFFFF, 0], device_id=UNIT)


def test_sunspec_marker(client: ModbusTcpClient) -> None:
    """SunSpec marker 'SunS' must be at 40000-40001."""
    assert read(client, 40000, 2) == SUNS


def test_model1_id(client: ModbusTcpClient) -> None:
    """Common block model ID must be 1."""
    assert read(client, 40002) == [1]


def test_model1_device_address(client: ModbusTcpClient) -> None:
    """DA register (40068) must report the configured unit address."""
    assert read(client, 40068) == [1]


def test_model101_id(client: ModbusTcpClient) -> None:
    """Inverter block model ID must be 101 (single phase) or 103 (three phase)."""
    assert read(client, 40070)[0] in (101, 103)


def test_model120_id(client: ModbusTcpClient) -> None:
    """Nameplate block model ID must be 120 with a non-zero rated power."""
    regs = read(client, 40122, 4)
    assert regs[0] == 120
    assert regs[3] > 0, "WRtg must not be 0"


def test_model123_id(client: ModbusTcpClient) -> None:
    """Controls block model ID must be 123."""
    assert read(client, 40150) == [123]


def test_end_model(client: ModbusTcpClient) -> None:
    """End model at 40176 must be ID 0xFFFF with length 0."""
    assert read(client, 40176, 2) == [0xFFFF, 0x0000]


def test_wmaxlimpct_default(client: ModbusTcpClient) -> None:
    """WMaxLimPct must default to 10000 (100.00 %, SF=-2)."""
    assert read(client, 40155) == [10000]


def test_wmaxlim_ena_default(client: ModbusTcpClient) -> None:
    """WMaxLim_Ena must default to 0."""
    assert read(client, 40159) == [0]


def test_conn_default(client: ModbusTcpClient) -> None:
    """Conn (40154) must default to 1 (connected)."""
    assert read(client, 40154) == [1]


def test_scale_factors(client: ModbusTcpClient) -> None:
    """Fixed scale factor registers must have expected values."""
    expected = {
        40076: 0xFFFE,  # A_SF = -2
        40083: 0xFFFF,  # V_SF = -1
        40087: 0xFFFE,  # Hz_SF = -2
        40096: 0,  # WH_SF
        40107: 0xFFFF,  # Tmp_SF = -1
        40173: 0xFFFE,  # WMaxLimPct_SF = -2
    }
    for addr, val in expected.items():
        reg = read(client, addr)[0]
        assert reg == val, f"Register {addr}: expected {hex(val)}, got {hex(reg)}"


def test_power_scale_factors_match(client: ModbusTcpClient) -> None:
    """W_SF and WRtg_SF share one scale factor in 0..2."""
    w_sf = read(client, 40085)[0]
    assert w_sf in (0, 1, 2)
    assert read(client, 40126) == [w_sf]


def test_fc03_reads_manufacturer(client: ModbusTcpClient) -> None:
    """FC03 should return manufacturer string from registers 40004-40019."""
    regs = read(client, 40004, 16)
    raw = bytes(b for r in regs for b in (r >> 8, r & 0xFF))
    assert raw.startswith(b"Solis"), f"Got: {raw}"


def test_fc03_out_of_range(client: ModbusTcpClient) -> None:
    """FC03 past the end model (>= 40178) must return exception code 0x02."""
    rr = client.read_holding_registers(40178, count=1, device_id=UNIT)
    assert rr.isError()
    assert rr.exception_code == 0x02


def test_fc03_too_many_registers(raw_request: Callable[..., bytes]) -> None:
    """FC03 requesting > 125 registers must return exception code 0x03."""
    # MBAP: txid=1, proto=0, len=6, unit=1 | PDU: FC03, start=40000, count=126
    reply = raw_request(bytes.fromhex("0001 0000 0006 01 03 9C40 007E"))
    assert reply == bytes.fromhex("0001 0000 0003 01 83 03")


def test_short_pdu_closes_connection(raw_request: Callable[..., bytes]) -> None:
    """A frame whose MBAP length can't hold unit ID + function code is rejected."""
    assert raw_request(bytes.fromhex("0001 0000 0000")) == b""


def test_fc04_reads_same_bank(client: ModbusTcpClient) -> None:
    """FC04 (read input registers) must serve the same register bank."""
    rr = client.read_input_registers(40000, count=2, device_id=UNIT)
    assert not rr.isError()
    assert rr.registers == SUNS


def test_any_unit_id_served(client: ModbusTcpClient) -> None:
    """Any unit ID must be served (clients probe various IDs during discovery)."""
    for uid in (42, 126, 0xFF):
        rr = client.read_holding_registers(40000, count=2, device_id=uid)
        assert not rr.isError(), f"unit {uid} not served"
        assert rr.registers == SUNS


def test_fc06_write_readonly_register_rejected(client: ModbusTcpClient) -> None:
    """FC06 write to a read-only register must return exception 0x02."""
    wr = client.write_register(40084, 999, device_id=UNIT)  # AC Power is read-only
    assert wr.isError()
    assert wr.exception_code == 0x02


def test_fc06_write_scale_factor_rejected(client: ModbusTcpClient) -> None:
    """FC06 write to WMaxLimPct_SF (40173) must return exception 0x02."""
    wr = client.write_register(40173, 0, device_id=UNIT)
    assert wr.isError()
    assert wr.exception_code == 0x02


def test_fc16_outside_writable_window_rejected(client: ModbusTcpClient) -> None:
    """FC16 spanning past the writable window (40152-40172) must return 0x02."""
    wr = client.write_registers(40172, [0, 0], device_id=UNIT)  # touches 40173 (SF)
    assert wr.isError()
    assert wr.exception_code == 0x02


def test_fc06_wmaxlimpct_above_100_rejected(client: ModbusTcpClient) -> None:
    """WMaxLimPct above 10000 (100.00 %) must return exception 0x03 and not be stored."""
    wr = client.write_register(40155, 10001, device_id=UNIT)
    assert wr.isError()
    assert wr.exception_code == 0x03
    assert read(client, 40155) == [10000]


def test_fc06_enable_flag_must_be_boolean(client: ModbusTcpClient) -> None:
    """WMaxLim_Ena only accepts 0 or 1."""
    wr = client.write_register(40159, 2, device_id=UNIT)
    assert wr.isError()
    assert wr.exception_code == 0x03
    assert read(client, 40159) == [0]


def test_fc16_invalid_value_changes_nothing(client: ModbusTcpClient) -> None:
    """An FC16 with one out-of-range value is rejected as a whole."""
    wr = client.write_registers(
        40155, [5000, 0xFFFF, 0xFFFF, 0xFFFF, 7], device_id=UNIT
    )
    assert wr.isError()
    assert wr.exception_code == 0x03
    assert read(client, 40155) == [10000]
    assert read(client, 40159) == [0]


def test_fc06_write_wmaxlimpct(client: ModbusTcpClient, restore_limit: None) -> None:
    """FC06 write to WMaxLimPct (40155) must succeed and read back."""
    wr = client.write_register(40155, 8000, device_id=UNIT)  # 80.00 %
    assert not wr.isError()
    assert read(client, 40155) == [8000]


def test_fc16_write_both_limit_registers(
    client: ModbusTcpClient, restore_limit: None
) -> None:
    """FC16: write WMaxLimPct=75 % and WMaxLim_Ena=1, verify both persist."""
    assert not client.write_registers(40155, [7500], device_id=UNIT).isError()
    assert not client.write_registers(40159, [1], device_id=UNIT).isError()
    assert read(client, 40155) == [7500]
    assert read(client, 40159) == [1]


def test_fc16_multi_register_write(
    client: ModbusTcpClient, restore_limit: None
) -> None:
    """FC16: write 5 registers in one call spanning 40155-40159; verify they persist."""
    wr = client.write_registers(
        40155, [6000, 0xFFFF, 0x0000, 0xFFFF, 1], device_id=UNIT
    )
    assert not wr.isError()
    assert read(client, 40155) == [6000]
    assert read(client, 40159) == [1]


def test_power_limit_enable(client: ModbusTcpClient, restore_limit: None) -> None:
    """Setting WMaxLim_Ena=1 with WMaxLimPct=60 % updates the SunSpec registers.

    Verifying the inverter-side write requires checking the inverter, which is out
    of scope for automated testing.
    """
    client.write_register(40155, 6000, device_id=UNIT)
    client.write_register(40159, 1, device_id=UNIT)
    assert read(client, 40155) == [6000]
    assert read(client, 40159) == [1]


def test_power_limit_disable_restores_full_power(
    client: ModbusTcpClient, restore_limit: None
) -> None:
    """Setting WMaxLim_Ena=0 after a limit disables it."""
    client.write_register(40155, 5000, device_id=UNIT)
    client.write_register(40159, 1, device_id=UNIT)
    client.write_register(40159, 0, device_id=UNIT)
    assert read(client, 40159) == [0]


def test_power_limit_revert_timer(client: ModbusTcpClient, restore_limit: None) -> None:
    """WMaxLimPct_RvrtTms: the limit is disabled when no new command arrives in time."""
    wr = client.write_registers(40155, [5000, 0xFFFF, 1, 0xFFFF, 1], device_id=UNIT)
    assert not wr.isError()
    assert read(client, 40159) == [1]
    time.sleep(1.5)
    assert read(client, 40159) == [0], "limit did not revert after RvrtTms=1 s"
