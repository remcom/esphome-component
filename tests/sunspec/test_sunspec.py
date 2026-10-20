"""
Integration tests -- require the device to be flashed and reachable.
Run: pytest tests/sunspec/test_sunspec.py --device-ip=<IP>
"""


def test_sunspec_marker(client):
    """SunSpec marker 'SunS' must be at 40000-40001."""
    rr = client.read_holding_registers(40000, 2, slave=1)
    assert not rr.isError()
    assert rr.registers == [0x5375, 0x6E53], f"Got {[hex(r) for r in rr.registers]}"


def test_model1_id(client):
    """Common block model ID must be 1."""
    rr = client.read_holding_registers(40002, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 1


def test_model1_device_address(client):
    """DA register (40068) must report the configured unit address."""
    rr = client.read_holding_registers(40068, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 1


def test_model101_id(client):
    """Inverter block model ID must be 101."""
    rr = client.read_holding_registers(40070, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 101


def test_model120_id(client):
    """Nameplate block model ID must be 120."""
    rr = client.read_holding_registers(40122, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 120


def test_model123_id(client):
    """Controls block model ID must be 123."""
    rr = client.read_holding_registers(40150, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 123


def test_end_model(client):
    """End model at 40176 must be ID 0xFFFF with length 0."""
    rr = client.read_holding_registers(40176, 2, slave=1)
    assert not rr.isError()
    assert rr.registers == [0xFFFF, 0x0000]


def test_wmaxlimpct_default(client):
    """WMaxLimPct must default to 10000 (100.00 %, SF=-2)."""
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 10000


def test_wmaxlim_ena_default(client):
    """WMaxLim_Ena must default to 0."""
    rr = client.read_holding_registers(40159, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 0


def test_conn_default(client):
    """Conn (40154) must default to 1 (connected)."""
    rr = client.read_holding_registers(40154, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 1


def test_scale_factors(client):
    """Scale factor registers must have expected values."""
    expected = {
        40076: 0xFFFE,  # A_SF = -2
        40083: 0xFFFF,  # V_SF = -1
        40085: 0,       # W_SF
        40087: 0xFFFE,  # Hz_SF = -2
        40096: 0,       # WH_SF
        40107: 0xFFFF,  # Tmp_SF = -1
        40173: 0xFFFE,  # WMaxLimPct_SF = -2
    }
    for addr, val in expected.items():
        rr = client.read_holding_registers(addr, 1, slave=1)
        assert not rr.isError()
        assert rr.registers[0] == val, \
            f"Register {addr}: expected {hex(val)}, got {hex(rr.registers[0])}"


def test_fc03_reads_manufacturer(client):
    """FC03 should return manufacturer string from registers 40004-40019."""
    rr = client.read_holding_registers(40004, 16, slave=1)
    assert not rr.isError()
    raw = bytes(b for r in rr.registers for b in (r >> 8, r & 0xFF))
    assert raw.startswith(b"Solis"), f"Got: {raw}"


def test_fc03_out_of_range(client):
    """FC03 past the end model (>= 40178) must return exception code 0x02."""
    rr = client.read_holding_registers(40178, 1, slave=1)
    assert rr.isError() or rr.function_code == 0x83


def test_fc03_too_many_registers(client):
    """FC03 requesting > 125 registers must return exception code 0x03."""
    rr = client.read_holding_registers(40000, 126, slave=1)
    assert rr.isError() or rr.function_code == 0x83


def test_fc04_reads_same_bank(client):
    """FC04 (read input registers) must serve the same register bank."""
    rr = client.read_input_registers(40000, 2, slave=1)
    assert not rr.isError()
    assert rr.registers == [0x5375, 0x6E53]


def test_any_unit_id_served(client):
    """Any unit ID must be served (clients probe various IDs during discovery)."""
    for uid in (42, 126, 0xFF):
        rr = client.read_holding_registers(40000, 2, slave=uid)
        assert not rr.isError(), f"unit {uid} not served"
        assert rr.registers == [0x5375, 0x6E53]


def test_fc06_write_wmaxlimpct(client):
    """FC06 write to WMaxLimPct (40155) must succeed and read back."""
    wr = client.write_register(40155, 8000, slave=1)  # 80.00 %
    assert not wr.isError()
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 8000
    # restore
    client.write_register(40155, 10000, slave=1)


def test_fc06_write_readonly_register_rejected(client):
    """FC06 write to a read-only register must return exception 0x02."""
    wr = client.write_register(40084, 999, slave=1)  # AC Power is read-only
    assert wr.isError() or wr.function_code == 0x86


def test_fc06_write_scale_factor_rejected(client):
    """FC06 write to WMaxLimPct_SF (40173) must return exception 0x02."""
    wr = client.write_register(40173, 0, slave=1)
    assert wr.isError() or wr.function_code == 0x86


def test_fc16_write_both_limit_registers(client):
    """FC16: write WMaxLimPct=75 % and WMaxLim_Ena=1, verify both persist."""
    wr1 = client.write_multiple_registers(40155, [7500], slave=1)
    assert not wr1.isError()
    wr2 = client.write_multiple_registers(40159, [1], slave=1)
    assert not wr2.isError()
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert rr.registers[0] == 7500
    rr = client.read_holding_registers(40159, 1, slave=1)
    assert rr.registers[0] == 1
    # cleanup
    client.write_multiple_registers(40159, [0], slave=1)
    client.write_multiple_registers(40155, [10000], slave=1)


def test_fc16_multi_register_write(client):
    """FC16: write 5 registers in one call spanning 40155-40159; verify they persist."""
    wr = client.write_multiple_registers(40155, [6000, 0xFFFF, 0x0000, 0xFFFF, 1], slave=1)
    assert not wr.isError()
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 6000
    rr = client.read_holding_registers(40159, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 1
    # restore defaults
    client.write_multiple_registers(40155, [10000, 0xFFFF, 0xFFFF, 0xFFFF, 0], slave=1)


def test_fc16_outside_writable_window_rejected(client):
    """FC16 spanning past the writable window (40152-40172) must return 0x02."""
    wr = client.write_multiple_registers(40172, [0, 0], slave=1)  # touches 40173 (SF)
    assert wr.isError() or wr.function_code == 0x90


def test_power_limit_enable(client):
    """Setting WMaxLim_Ena=1 with WMaxLimPct=60 % should queue a limit command."""
    # This test verifies the SunSpec registers update correctly.
    # Verifying the Solis RS485 register write requires checking the inverter,
    # which is out of scope for automated testing.
    client.write_register(40155, 6000, slave=1)
    client.write_register(40159, 1, slave=1)
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 6000
    rr = client.read_holding_registers(40159, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 1
    # cleanup
    client.write_register(40159, 0, slave=1)
    client.write_register(40155, 10000, slave=1)


def test_power_limit_disable_restores_full_power(client):
    """Setting WMaxLim_Ena=0 after a limit should log a restore command."""
    client.write_register(40155, 5000, slave=1)
    client.write_register(40159, 1, slave=1)
    client.write_register(40159, 0, slave=1)
    rr = client.read_holding_registers(40159, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 0
    # cleanup
    client.write_register(40155, 10000, slave=1)
    rr = client.read_holding_registers(40155, 1, slave=1)
    assert not rr.isError()
    assert rr.registers[0] == 10000
