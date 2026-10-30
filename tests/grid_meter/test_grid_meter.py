"""
Integration tests for grid_meter component.
Requires a device running grid_meter, reachable on the network (or the host build of
tests/components/grid_meter/test.host.yaml, see README).

Run: pytest tests/grid_meter/test_grid_meter.py --device-ip=<IP> -v
"""

from collections.abc import Callable
import struct
import time

from pymodbus.client import ModbusTcpClient

UNIT = 1


def read_int32(registers: list[int], idx: int) -> int:
    """Reconstruct signed int32 from two uint16 registers using Reg_s32l word order.

    EM24 uses little-endian word order: low word at lower address (idx), high word at idx+1.
    pymodbus returns registers in address order, so registers[idx] = low word.
    """
    raw = (registers[idx + 1] << 16) | registers[idx]
    return struct.unpack(">i", struct.pack(">I", raw))[0]


def read_uint32(registers: list[int], idx: int) -> int:
    """Reconstruct uint32 from two uint16 registers using Reg_s32l word order."""
    return (registers[idx + 1] << 16) | registers[idx]


def test_fc03_reads_registers(client: ModbusTcpClient) -> None:
    """FC03 reading a block of registers from 0x0000 must succeed."""
    rr = client.read_holding_registers(0x0000, count=20, device_id=UNIT)
    assert not rr.isError(), f"FC03 failed: {rr}"
    assert len(rr.registers) == 20


def test_voltage_is_plausible(client: ModbusTcpClient) -> None:
    """L1 voltage must be in range 200-260 V (typical Dutch grid)."""
    rr = client.read_holding_registers(0x0000, count=2, device_id=UNIT)
    assert not rr.isError()
    voltage = read_int32(rr.registers, 0) / 10.0  # x0.1 V
    assert 200.0 <= voltage <= 260.0, f"Voltage out of range: {voltage} V"


def test_current_is_non_negative(client: ModbusTcpClient) -> None:
    """L1 current must be >= 0 (always positive magnitude). Register 0x000C-0x000D."""
    rr = client.read_holding_registers(0x000C, count=2, device_id=UNIT)
    assert not rr.isError()
    current = read_int32(rr.registers, 0) / 1000.0  # x0.001 A
    assert current >= 0.0, f"Current is negative: {current} A"


def test_power_is_signed_int32(client: ModbusTcpClient) -> None:
    """Active power register pair must decode as a valid signed int32. Register 0x0012-0x0013."""
    rr = client.read_holding_registers(0x0012, count=2, device_id=UNIT)
    assert not rr.isError()
    power = read_int32(rr.registers, 0) / 10.0
    assert -100000.0 < power < 100000.0, f"Power implausible: {power} W"


def test_model_id_register(client: ModbusTcpClient) -> None:
    """Register 0x000B must contain EM24 device ID (1648)."""
    rr = client.read_holding_registers(0x000B, count=1, device_id=UNIT)
    assert not rr.isError()
    assert rr.registers[0] == 1648, f"Unexpected model ID: {rr.registers[0]}"


def test_energy_import_decodes(client: ModbusTcpClient) -> None:
    """Energy import total must decode. Register 0x0034-0x0035."""
    rr = client.read_holding_registers(0x0034, count=2, device_id=UNIT)
    assert not rr.isError()
    assert read_uint32(rr.registers, 0) >= 0


def test_energy_export_decodes(client: ModbusTcpClient) -> None:
    """Energy export total must decode. Register 0x004E-0x004F."""
    rr = client.read_holding_registers(0x004E, count=2, device_id=UNIT)
    assert not rr.isError()
    assert read_uint32(rr.registers, 0) >= 0


def test_l1_energy_mirrors_totals(client: ModbusTcpClient) -> None:
    """L1 import (0x0040) and export (0x0046) mirror the totals (DSMR has no per-phase energy)."""
    rr = client.read_holding_registers(0x0034, count=0x50 - 0x34, device_id=UNIT)
    assert not rr.isError()
    regs = rr.registers
    assert regs[0x40 - 0x34 : 0x42 - 0x34] == regs[0:2]
    assert regs[0x46 - 0x34 : 0x48 - 0x34] == regs[0x4E - 0x34 : 0x50 - 0x34]


def test_identification_registers(client: ModbusTcpClient) -> None:
    """Sparse identification registers expected by the Cerbo's EM24 driver."""
    for addr, expected in {
        0xA000: 7,
        0xA100: 2,
        0x0302: 0x0100,
        0x0304: 0x0100,
    }.items():
        rr = client.read_holding_registers(addr, count=1, device_id=UNIT)
        assert not rr.isError()
        assert rr.registers[0] == expected, f"0x{addr:04X}: {rr.registers[0]}"
    rr = client.read_holding_registers(0x1002, count=1, device_id=UNIT)
    assert not rr.isError()
    assert rr.registers[0] in (0, 3), f"PhaseConfig: {rr.registers[0]}"


def test_fc04_reads_same_as_fc03(client: ModbusTcpClient) -> None:
    """FC04 must return the same stable register values as FC03 (Cerbo may use either).

    Only voltage (0x0000-0x0001) and model ID (0x000B) are compared, since power can
    legitimately change between the two consecutive reads.
    """
    rr03 = client.read_holding_registers(0x0000, count=12, device_id=UNIT)
    rr04 = client.read_input_registers(0x0000, count=12, device_id=UNIT)
    assert not rr03.isError() and not rr04.isError()
    assert rr03.registers[0:2] == rr04.registers[0:2], (
        f"Voltage differs between FC03 and FC04: {rr03.registers[0:2]} vs {rr04.registers[0:2]}"
    )
    assert rr03.registers[11] == rr04.registers[11], (
        f"Model ID differs between FC03 and FC04: {rr03.registers[11]} vs {rr04.registers[11]}"
    )


def test_fc03_out_of_range_returns_zero(client: ModbusTcpClient) -> None:
    """FC03 at address 0x0100 (outside dense range, not in sparse table) must succeed and return 0."""
    rr = client.read_holding_registers(0x0100, count=1, device_id=UNIT)
    assert not rr.isError(), f"FC03 out-of-range unexpectedly failed: {rr}"
    assert rr.registers[0] == 0, (
        f"Expected 0 for unknown register, got {rr.registers[0]}"
    )


def test_fc03_too_many_registers_returns_exception(
    raw_request: Callable[..., bytes],
) -> None:
    """FC03 with count > 125 must return exception 0x03 (Illegal Data Value)."""
    # MBAP: txid=1, proto=0, len=6, unit=1 | PDU: FC03, start=0, count=126
    reply = raw_request(bytes.fromhex("0001 0000 0006 01 03 0000 007E"))
    assert reply == bytes.fromhex("0001 0000 0003 01 83 03")


def test_short_pdu_closes_connection(raw_request: Callable[..., bytes]) -> None:
    """A frame whose MBAP length can't hold unit ID + function code is rejected."""
    assert raw_request(bytes.fromhex("0001 0000 0001 01")) == b""


def test_fc06_write_accepted_as_noop(client: ModbusTcpClient) -> None:
    """FC06 write must succeed as a no-op (component echoes the request, ignores the write)."""
    before = client.read_holding_registers(0x0004, count=1, device_id=UNIT)
    wr = client.write_register(0x0004, 0x1234, device_id=UNIT)
    assert not wr.isError(), f"FC06 unexpectedly returned an error: {wr}"
    after = client.read_holding_registers(0x0004, count=1, device_id=UNIT)
    assert after.registers == before.registers


def test_readings_are_stable_between_polls(client: ModbusTcpClient) -> None:
    """Voltage and energy must not change between rapid polls (within 0.5 s)."""
    v1 = client.read_holding_registers(0x0000, count=2, device_id=UNIT)
    ei1 = client.read_holding_registers(0x0034, count=2, device_id=UNIT)
    time.sleep(0.5)
    v2 = client.read_holding_registers(0x0000, count=2, device_id=UNIT)
    ei2 = client.read_holding_registers(0x0034, count=2, device_id=UNIT)
    assert not v1.isError() and not v2.isError()
    assert not ei1.isError() and not ei2.isError()
    assert v1.registers == v2.registers, (
        f"Voltage changed between polls: {v1.registers} vs {v2.registers}"
    )
    assert ei1.registers == ei2.registers, (
        f"Energy import changed between polls: {ei1.registers} vs {ei2.registers}"
    )
