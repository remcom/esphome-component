"""Integration tests: the host build talks to a fake radar on a pseudo-terminal.

The host build opens /tmp/aqara-fp2-radar as its UART; each test links that path to a fresh
pseudo-terminal, starts the binary and plays the radar. Entity states are read from the binary's
VERBOSE log, so this needs ESPHome >= 2026.9 (older host loggers do not flush stdout).

Build:  esphome compile tests/components/aqara_fp2/test.host.yaml
Run:    pytest tests/aqara_fp2/test_aqara_fp2.py --aqara-fp2-binary=<program> -v
"""

from collections.abc import Iterator
import contextlib
from dataclasses import dataclass
import os
from pathlib import Path
import re
import select
import subprocess
import threading
import time
import tty

import pytest

PTY_LINK = Path("/tmp/aqara-fp2-radar")
# radar_timeout in tests/components/aqara_fp2/test.host.yaml
RADAR_TIMEOUT_S = 3

OP_RESPONSE = 0x01
OP_WRITE = 0x02
OP_ACK = 0x03
OP_READ = 0x04
OP_REPORT = 0x05

TYPE_UINT8 = 0x00
TYPE_UINT16 = 0x01
TYPE_BINARY = 0x06

ATTR_RADAR_SW_VERSION = 0x0102
ATTR_PRESENCE_DETECT = 0x0104
ATTR_MONITOR_MODE = 0x0105
ATTR_ZONE_MAP = 0x0114
ATTR_LOCATION_TRACKING_DATA = 0x0117
ATTR_ANGLE_SENSOR_DATA = 0x0120
ATTR_TEMPERATURE = 0x0128
ATTR_ZONE_PRESENCE = 0x0142
ATTR_DEVICE_DIRECTION = 0x0143

# Configuration writes for tests/components/aqara_fp2/common.yaml (interference grid, one zone).
EXPECTED_CONFIG = [
    ATTR_MONITOR_MODE,
    0x0122,  # left/right reverse
    0x0111,  # presence sensitivity
    0x0106,  # closing setting
    0x0153,  # close/away enable
    0x0158,  # people count report
    0x0162,  # people number
    0x0163,  # target type
    0x0170,  # mounting position
    0x0172,  # dwell time
    0x0173,  # walk distance
    0x0138,  # thermo enable
    0x0141,  # thermo data
    0x0112,  # location report enable
    0x0110,  # interference map
    ATTR_ZONE_MAP,
    0x0151,  # zone 1 sensitivity
    0x0202,  # zone activation list
    0x0153,  # zone 1 close/away enable
]


def crc16(data: bytes) -> int:
    """CRC16/MODBUS."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_frame(op: int, payload: bytes, seq: int = 0) -> bytes:
    header = bytes([0x55, 0x00, 0x01, seq, op, len(payload) >> 8, len(payload) & 0xFF])
    header += bytes([(~(sum(header) - 1)) & 0xFF])
    crc = crc16(header + payload)
    return header + payload + bytes([crc & 0xFF, crc >> 8])


def attribute(attr: int, data_type: int, value: bytes = b"") -> bytes:
    return attr.to_bytes(2, "big") + bytes([data_type]) + value


@dataclass
class Frame:
    op: int
    payload: bytes

    @property
    def attr(self) -> int:
        return int.from_bytes(self.payload[:2], "big")


class FakeRadar:
    def __init__(self, binary: str) -> None:
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        PTY_LINK.unlink(missing_ok=True)
        PTY_LINK.symlink_to(os.ttyname(self.slave))
        self.rx = bytearray()
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
            self.wait_log(r"Aqara FP2:", timeout=10)
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

    def send(self, op: int, payload: bytes) -> None:
        os.write(self.master, build_frame(op, payload))

    def report(self, attr: int, data_type: int, value: bytes = b"") -> None:
        self.send(OP_REPORT, attribute(attr, data_type, value))

    def heartbeat(self, version: int = 5) -> None:
        self.report(ATTR_RADAR_SW_VERSION, TYPE_UINT8, bytes([version]))

    def ack(self, frame: Frame) -> None:
        self.send(OP_ACK, attribute(frame.attr, 0x03))

    def _parse(self) -> Frame | None:
        start = self.rx.find(b"\x55\x00\x01")
        if start < 0:
            del self.rx[:-2]
            return None
        del self.rx[:start]
        if len(self.rx) < 8:
            return None
        length = int.from_bytes(self.rx[5:7], "big")
        if len(self.rx) < 8 + length + 2:
            return None
        frame = bytes(self.rx[: 8 + length + 2])
        del self.rx[: len(frame)]
        assert frame[7] == (~(sum(frame[:7]) - 1)) & 0xFF, "bad header checksum"
        assert int.from_bytes(frame[-2:], "little") == crc16(frame[:-2]), "bad CRC"
        return Frame(op=frame[4], payload=frame[8:-2])

    def read_frame(self, timeout: float) -> Frame | None:
        deadline = time.monotonic() + timeout
        while (frame := self._parse()) is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if select.select([self.master], [], [], remaining)[0]:
                self.rx += os.read(self.master, 1024)
        return frame

    def expect_frame(self, timeout: float = 2.0) -> Frame:
        frame = self.read_frame(timeout)
        assert frame is not None, "no frame from the ESP"
        return frame

    def expect_silence(self, duration: float) -> None:
        frame = self.read_frame(duration)
        assert frame is None, (
            f"unexpected frame: op {frame.op}, attr 0x{frame.attr:04X}"
        )

    def wait_log(self, pattern: str, timeout: float = 2.0) -> re.Match:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self.log_changed:
            while True:
                for line in self.log:
                    if match := regex.search(line):
                        return match
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.proc.poll() is not None:
                    pytest.fail(f"log line matching {pattern!r} not found")
                self.log_changed.wait(remaining)

    def configure(self) -> list[Frame]:
        """Send a heartbeat and acknowledge the configuration writes."""
        self.heartbeat()
        frames = []
        while (frame := self.read_frame(1.0)) is not None:
            assert frame.op == OP_WRITE
            frames.append(frame)
            self.ack(frame)
        self.wait_log(r"Radar configured")
        return frames


@pytest.fixture
def radar(request: pytest.FixtureRequest) -> Iterator[FakeRadar]:
    binary = request.config.getoption("--aqara-fp2-binary")
    if binary is None:
        pytest.skip("--aqara-fp2-binary not provided")
    fake = FakeRadar(binary)
    yield fake
    with contextlib.suppress(OSError):
        fake.close()


def test_nothing_sent_before_heartbeat(radar: FakeRadar) -> None:
    radar.expect_silence(1.0)


def test_configuration_sent_one_write_at_a_time(radar: FakeRadar) -> None:
    radar.heartbeat()
    frames = []
    for _ in EXPECTED_CONFIG:
        frame = radar.expect_frame()
        assert frame.op == OP_WRITE
        # The next write waits for this one's ACK.
        radar.expect_silence(0.2)
        frames.append(frame)
        radar.ack(frame)
    radar.expect_silence(0.5)
    radar.wait_log(r"Radar configured")
    radar.wait_log(r"'Radar version' >> '5'")

    assert [frame.attr for frame in frames] == EXPECTED_CONFIG
    zone_map = next(frame for frame in frames if frame.attr == ATTR_ZONE_MAP)
    # type, 16-bit length (41), zone id 1, then rows 0-2 with input columns 2-4 set (bits 11-9).
    assert zone_map.payload[2:6] == bytes([TYPE_BINARY, 0, 41, 1])
    assert zone_map.payload[6:12] == bytes([0x0E, 0x00] * 3)
    assert not any(zone_map.payload[12:])


def test_reports_acknowledged_and_published(radar: FakeRadar) -> None:
    radar.configure()

    radar.report(ATTR_PRESENCE_DETECT, TYPE_UINT8, b"\x01")
    ack = radar.expect_frame()
    assert (ack.op, ack.attr) == (OP_ACK, ATTR_PRESENCE_DETECT)
    radar.wait_log(r"'Presence' >> ON")

    radar.report(ATTR_ZONE_PRESENCE, TYPE_UINT16, b"\x01\x01")
    assert radar.expect_frame().op == OP_ACK
    radar.wait_log(r"'Sofa presence' >> ON")

    radar.report(ATTR_TEMPERATURE, TYPE_UINT16, (23).to_bytes(2, "big"))
    assert radar.expect_frame().op == OP_ACK
    radar.wait_log(r"'Radar temperature' >> 23")


def test_location_report_publishes_targets(radar: FakeRadar) -> None:
    radar.configure()
    record = bytes([0])
    for value in (100, -200, 1500, -30):  # x, y, z, speed
        record += value.to_bytes(2, "big", signed=True)
    record += bytes(5)  # snr, classifier, posture, active
    blob = bytes([1]) + record
    radar.report(
        ATTR_LOCATION_TRACKING_DATA, TYPE_BINARY, len(blob).to_bytes(2, "big") + blob
    )

    assert radar.expect_frame().op == OP_ACK
    radar.wait_log(r"'Target 1 X' >> 100 mm")
    radar.wait_log(r"'Target 1 Y' >> -200 mm")
    radar.wait_log(r"'Target 1 speed' >> -30 mm/s")
    radar.wait_log(r"'Target count' >> 1")
    radar.wait_log(r"'Has target' >> ON")


def test_corrupt_frame_ignored(radar: FakeRadar) -> None:
    frame = bytearray(
        build_frame(OP_REPORT, attribute(ATTR_PRESENCE_DETECT, TYPE_UINT8, b"\x01"))
    )
    frame[-1] ^= 0xFF
    os.write(radar.master, frame)
    radar.expect_silence(0.5)
    radar.wait_log(r"Frame CRC mismatch")


@pytest.mark.parametrize("attr", [ATTR_DEVICE_DIRECTION, ATTR_ANGLE_SENSOR_DATA])
def test_radar_query_answered(radar: FakeRadar, attr: int) -> None:
    """Without an accelerometer the answer is "upright" / 0 degrees."""
    radar.send(OP_RESPONSE, attr.to_bytes(2, "big"))
    answer = radar.expect_frame()
    assert answer.op == OP_READ
    assert answer.payload == attribute(attr, TYPE_UINT8, b"\x00")


def test_unacknowledged_configuration_restarts_on_heartbeat(radar: FakeRadar) -> None:
    radar.heartbeat()
    first = radar.expect_frame()
    assert first.attr == ATTR_MONITOR_MODE
    # Sent three times in total, then given up.
    for _ in range(2):
        assert radar.expect_frame(timeout=1.0).payload == first.payload
    radar.expect_silence(1.0)
    radar.wait_log(r"Radar did not acknowledge 0x0105")

    radar.heartbeat()
    assert radar.expect_frame().attr == ATTR_MONITOR_MODE


def test_watchdog_reconfigures_after_silence(radar: FakeRadar) -> None:
    radar.configure()
    radar.wait_log(r"No data from radar", timeout=RADAR_TIMEOUT_S + 3)
    radar.wait_log(r"'Presence' >> UNKNOWN")

    radar.heartbeat()
    assert radar.expect_frame().attr == ATTR_MONITOR_MODE
