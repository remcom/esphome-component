"""Integration tests: the host build listens to a fake heat pump bus on a pseudo-terminal.

The host build opens /tmp/thermia-itec-eco-bus as its UART; each test links that path to a fresh
pseudo-terminal, starts the binary and plays the display panel and the heat pump's boards. Entity
states are read from the binary's VERBOSE log; the controls (room sensor switch, setpoint number,
top-up button) are driven over the native API.

Build:  esphome compile tests/components/thermia_itec_eco/test.host.yaml
Run:    pytest tests/thermia_itec_eco/test_thermia_itec_eco.py \
            --thermia-itec-eco-build-dir=tests/components/thermia_itec_eco/.esphome/build -v
"""

import asyncio
from collections.abc import Iterator
import contextlib
import os
from pathlib import Path
import re
import select
import subprocess
import threading
import time
import tty

from aioesphomeapi import APIClient
import pytest

PTY_LINK = Path("/tmp/thermia-itec-eco-bus")
API_PORT = 6063  # tests/components/thermia_itec_eco/test.host.yaml
BUS_TIMEOUT_S = 3  # bus_timeout in test.host.yaml
DEVICE = "thermia-itec-eco-host"
CAPTURE = Path(__file__).parent / "itec_eco5_capture.log"

ROOM_SENSOR = 0x0A
REPLY_LEN = 11


def crc16(data: bytes) -> int:
    """CRC16/MODBUS."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(data: bytes) -> bytes:
    return data + crc16(data).to_bytes(2, "little")


def words(*values: int) -> bytes:
    return b"".join((value & 0xFFFF).to_bytes(2, "big") for value in values)


def fc23_request(
    slave: int, read_start: int, read_count: int, write_start: int, values: list[int]
) -> bytes:
    header = bytes([slave, 0x17]) + words(read_start, read_count, write_start)
    header += words(len(values)) + bytes([2 * len(values)])
    return with_crc(header + words(*values))


def read_reply(slave: int, function: int, values: list[int]) -> bytes:
    return with_crc(bytes([slave, function, 2 * len(values)]) + words(*values))


def fc16_request(slave: int, start: int, values: list[int]) -> bytes:
    header = bytes([slave, 0x10]) + words(start, len(values)) + bytes([2 * len(values)])
    return with_crc(header + words(*values))


def room_sensor_poll(outdoor: int = 5, setpoint: int = 20, top_up: int = 0) -> bytes:
    return fc23_request(ROOM_SENSOR, 0xB3B0, 3, 0xB3C4, [outdoor & 0xFF, setpoint, top_up])


class FakeBus:
    def __init__(self, binary: str) -> None:
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        PTY_LINK.unlink(missing_ok=True)
        PTY_LINK.symlink_to(os.ttyname(self.slave))
        self.log: list[str] = []
        self.log_changed = threading.Condition()
        self.proc = subprocess.Popen(
            [binary],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        threading.Thread(target=self._read_log, daemon=True).start()
        try:
            self.wait_log(r"Thermia iTec Eco:", timeout=10)
        except BaseException:
            self.close()
            raise

    def _read_log(self) -> None:
        ansi = re.compile(r"\x1b\[[0-9;]*m")
        for line in self.proc.stdout:
            with self.log_changed:
                self.log.append(ansi.sub("", line.rstrip()))
                self.log_changed.notify_all()

    def close(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        os.close(self.master)
        os.close(self.slave)
        PTY_LINK.unlink(missing_ok=True)

    def send(self, *frames: bytes) -> None:
        for frame in frames:
            os.write(self.master, frame)
            # Frames on the real bus are tens of milliseconds apart.
            time.sleep(0.02)

    def receive(self, timeout: float) -> bytes:
        data = b""
        deadline = time.monotonic() + timeout
        while len(data) < REPLY_LEN:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.master], [], [], remaining)[0]:
                break
            data += os.read(self.master, 64)
        return data

    def poll_room_sensor(self, **kwargs: int) -> bytes:
        """Poll the room sensor slot like the display; returns the reply (b"" if none)."""
        os.write(self.master, room_sensor_poll(**kwargs))
        return self.receive(0.5)

    def wait_log(self, pattern: str, timeout: float = 2.0, start: int = 0) -> re.Match:
        """Wait for a log line matching `pattern`, looking from line `start` on."""
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self.log_changed:
            while True:
                for line in self.log[start:]:
                    if match := regex.search(line):
                        return match
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.proc.poll() is not None:
                    pytest.fail(f"log line matching {pattern!r} not found")
                self.log_changed.wait(remaining)


def parse_reply(reply: bytes) -> tuple[float, int, int]:
    """Room sensor reply -> (temperature, setpoint request, top-up request)."""
    assert len(reply) == REPLY_LEN, f"reply {reply.hex()}"
    assert reply[:3] == bytes([ROOM_SENSOR, 0x17, 6])
    assert int.from_bytes(reply[-2:], "little") == crc16(reply[:-2]), "bad CRC"
    temperature = int.from_bytes(reply[3:5], "big", signed=True) / 10
    return (
        temperature,
        int.from_bytes(reply[5:7], "big"),
        int.from_bytes(reply[7:9], "big"),
    )


def api(command: str, name: str, *args: object) -> None:
    """Send `command` (switch/number/button) to the entity called `name` over the native API."""

    async def run() -> None:
        client = APIClient("127.0.0.1", API_PORT, None)
        await client.connect(login=True)
        try:
            entities, _ = await client.list_entities_services()
            key = next(entity.key for entity in entities if entity.name == name)
            getattr(client, f"{command}_command")(key, *args)
            # Commands are fire-and-forget; give the device time to handle it before closing.
            await asyncio.sleep(0.3)
        finally:
            await client.disconnect()

    asyncio.run(run())


@pytest.fixture
def bus(request: pytest.FixtureRequest) -> Iterator[FakeBus]:
    build_dir = request.config.getoption("--thermia-itec-eco-build-dir")
    if build_dir is None:
        pytest.skip("--thermia-itec-eco-build-dir not provided")
    binary = Path(build_dir) / DEVICE / ".pioenvs" / DEVICE / "program"
    assert binary.is_file(), f"{binary} not built"
    fake = FakeBus(str(binary))
    yield fake
    with contextlib.suppress(OSError):
        fake.close()


@pytest.fixture
def emulating(bus: FakeBus) -> FakeBus:
    """Room sensor emulation switched on (it starts off)."""
    api("switch", "Room sensor emulation", True)
    bus.wait_log(r"Room sensor emulation enabled")
    return bus


def test_capture_decoded(bus: FakeBus) -> None:
    """30 s of a real iTec Eco 5 bus, with an online gateway (thermia-bus-sniffer)."""
    for line in CAPTURE.read_text().splitlines():
        if line.startswith("#"):
            continue
        bus.send(bytes.fromhex(line.split()[1]))

    expected = {
        "Supply temperature": "24",
        "Hot water top temperature": "55",
        "Hot water temperature": "41",
        "Outdoor temperature": "16",
        "Room setpoint": "20",
        "Condenser inlet temperature": "23.1",
        "Condenser outlet temperature": "22.6",
        "Expansion valve position": "480",
        "Compressor frequency": "0",
        "Circulation pump speed": "60",
        "Target supply temperature": "0.0",
    }
    for name, value in expected.items():
        bus.wait_log(rf"'{name}' >> {re.escape(value)} ")
    for name, state in {
        "Compressor": "OFF",
        "Circulation pump": "ON",
        "Outdoor fan": "OFF",
        "Defrost": "OFF",
        "Hot water valve": "OFF",
        "Immersion heater 1": "OFF",
        "Hot water top-up": "OFF",
    }.items():
        bus.wait_log(rf"'{name}' >> {state}")
    bus.wait_log(r"'Operating mode' >> 'Heating'")
    # High pressure reads 0xFFFF while the compressor is idle: not available.
    bus.wait_log(r"'High pressure' >> nan")
    # The room sensor slot is polled, but emulation is off: nothing transmitted.
    assert bus.receive(0.2) == b""


def test_controller_values(bus: FakeBus) -> None:
    """Temperatures, -1000 for an absent probe, and the output bits."""
    bus.send(
        fc23_request(0x02, 0xA7F8, 15, 0xA80C, [0x81, 0x10, 0, 85, 0, 0, 0]),
        read_reply(0x02, 0x17, [35, 0, 0, 52, -1000, 0, 0, 0, 0, 0, -7, 0, 0, 0, 0]),
    )
    bus.wait_log(r"'Supply temperature' >> 35 ")
    bus.wait_log(r"'Hot water top temperature' >> 52 ")
    bus.wait_log(r"'Hot water temperature' >> nan")
    bus.wait_log(r"'Outdoor temperature' >> -7 ")
    bus.wait_log(r"'Hot water valve' >> ON")
    bus.wait_log(r"'Immersion heater 1' >> ON")
    bus.wait_log(r"'Immersion heater 2' >> ON")
    bus.wait_log(r"'External aux heater' >> OFF")
    bus.wait_log(r"'Circulation pump speed' >> 85 ")


def test_outdoor_unit_values(bus: FakeBus) -> None:
    unit = [0] * 22
    unit[0], unit[1], unit[8] = 301, 352, 245  # condenser in/out (0.1 °C), high pressure (0.1 bar)
    unit[11], unit[13], unit[14], unit[16] = 42, 63, 610, 320  # Hz, 0.1 A, rpm, steps
    unit[20], unit[21] = 0x1D, 0x0204  # compressor + fan + pump; defrost
    bus.send(
        with_crc(bytes([0x1E, 0x04]) + words(0, 22)),
        read_reply(0x1E, 0x04, unit),
        fc16_request(0x1E, 0, [1, 345, 0, 0, 2, 0, 0, 1, 1]),
    )
    bus.wait_log(r"'Condenser inlet temperature' >> 30.1 ")
    bus.wait_log(r"'Condenser outlet temperature' >> 35.2 ")
    bus.wait_log(r"'High pressure' >> 24.5 ")
    bus.wait_log(r"'Compressor frequency' >> 42 ")
    bus.wait_log(r"'Compressor current' >> 6.3 ")
    bus.wait_log(r"'Outdoor fan speed' >> 610 ")
    bus.wait_log(r"'Expansion valve position' >> 320 ")
    bus.wait_log(r"'Compressor' >> ON")
    bus.wait_log(r"'Outdoor fan' >> ON")
    bus.wait_log(r"'Defrost' >> ON")
    bus.wait_log(r"'Target supply temperature' >> 34.5 ")
    bus.wait_log(r"'Operating mode' >> 'Hot water'")


def test_settings_page(bus: FakeBus) -> None:
    bus.send(
        fc16_request(0x0F, 1000, [30, 25, 55, -2, 1, 3, 17, 18, 2, 40, 30, 1, 21, 2])
    )
    for name, value in {
        "Heating curve": "30",
        "Heating curve min": "25",
        "Heating curve max": "55",
        "Curve correction \\+5": "-2",
        "Curve correction 0": "1",
        "Curve correction -5": "3",
        "Heating stop temperature": "17",
        "Setback temperature": "18",
        "Room factor": "2",
    }.items():
        bus.wait_log(rf"'{name}' >> {value} ")
    bus.wait_log(r"'High power' >> ON")


def test_reply_needs_switch(bus: FakeBus) -> None:
    assert bus.poll_room_sensor() == b""


def test_room_sensor_reply(emulating: FakeBus) -> None:
    temperature, setpoint, top_up = parse_reply(emulating.poll_room_sensor())
    assert (temperature, setpoint, top_up) == (21.5, 0, 0)
    emulating.wait_log(r"'Room temperature' >> 21.5 ")

    api("number", "Room temperature input", 18.7)
    time.sleep(1.5)  # the template sensor updates every second
    assert parse_reply(emulating.poll_room_sensor())[0] == 18.7

    # An implausible room temperature (outside 0-40 °C) is not passed on.
    api("number", "Room temperature input", -2.3)
    time.sleep(1.5)
    assert emulating.poll_room_sensor() == b""

    api("switch", "Room sensor emulation", False)
    emulating.wait_log(r"Room sensor emulation disabled")
    assert emulating.poll_room_sensor() == b""


def test_setpoint_request(emulating: FakeBus) -> None:
    assert parse_reply(emulating.poll_room_sensor(setpoint=20))[1] == 0
    emulating.wait_log(r"'Room setpoint request' >> 20")

    api("number", "Room setpoint request", 22)
    # Requested until the controller pushes the new setpoint back.
    assert parse_reply(emulating.poll_room_sensor(setpoint=20))[1] == 22
    assert parse_reply(emulating.poll_room_sensor(setpoint=20))[1] == 22
    assert parse_reply(emulating.poll_room_sensor(setpoint=22))[1] == 0
    emulating.wait_log(r"Controller adopted room setpoint 22")
    emulating.wait_log(r"'Room setpoint' >> 22 ")


def test_setpoint_request_gives_up(emulating: FakeBus) -> None:
    emulating.poll_room_sensor(setpoint=20)
    api("number", "Room setpoint request", 24)
    requests = [parse_reply(emulating.poll_room_sensor(setpoint=20))[1] for _ in range(7)]
    assert requests == [24] * 5 + [0, 0]
    emulating.wait_log(r"Controller did not adopt room setpoint 24")
    # The number shows the controller's setpoint again.
    emulating.wait_log(r"'Room setpoint request' >> 20")


def test_hot_water_top_up(emulating: FakeBus) -> None:
    assert parse_reply(emulating.poll_room_sensor())[2] == 0
    api("button", "Start hot water top-up")
    # One-shot: only the next reply carries the request.
    assert parse_reply(emulating.poll_room_sensor())[2] == 1
    assert parse_reply(emulating.poll_room_sensor(top_up=2))[2] == 0
    emulating.wait_log(r"'Hot water top-up' >> ON")


def test_other_room_sensor_blocks_emulation(bus: FakeBus) -> None:
    """A real room sensor answering the poll stops the emulation for good."""
    bus.send(room_sensor_poll(), read_reply(ROOM_SENSOR, 0x17, [198, 0, 0]))
    bus.wait_log(r"Another room sensor answers on the bus")
    bus.wait_log(r"'Room temperature' >> 19.8 ")
    api("switch", "Room sensor emulation", True)
    assert bus.poll_room_sensor() == b""


def test_garbage_and_split_frames(bus: FakeBus) -> None:
    """Noise between frames and frames arriving in pieces are handled."""
    request = fc23_request(0x02, 0xA7F8, 15, 0xA80C, [0x40, 0, 0, 30, 0, 0, 0])
    reply = read_reply(0x02, 0x17, [27, 0, 0, 50, 45, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0])
    bad_crc = bytearray(reply)
    bad_crc[5] ^= 0xFF
    bus.send(b"\x00\xff\x17\x02", bytes(bad_crc), request[:7])
    bus.send(request[7:] + reply[:10])
    bus.send(reply[10:])
    bus.wait_log(r"'Supply temperature' >> 27 ")
    bus.wait_log(r"'Outdoor temperature' >> 3 ")
    assert not any("'Supply temperature' >> " in line and ">> 27 " not in line for line in bus.log)


def test_bus_timeout(bus: FakeBus) -> None:
    bus.send(
        fc23_request(0x02, 0xA7F8, 15, 0xA80C, [0x40, 0, 0, 30, 0, 0, 0]),
        read_reply(0x02, 0x17, [33, 0, 0, 50, 45, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0]),
    )
    bus.wait_log(r"'Supply temperature' >> 33 ")
    start = len(bus.log)
    bus.wait_log(r"No valid frame from the heat pump", timeout=BUS_TIMEOUT_S + 2, start=start)
    bus.wait_log(r"'Supply temperature' >> nan", start=start)
    bus.wait_log(r"'Hot water valve' >> UNKNOWN|'Hot water valve' >> unknown", start=start)

    bus.send(room_sensor_poll())
    bus.wait_log(r"Receiving heat pump data again", start=start)
