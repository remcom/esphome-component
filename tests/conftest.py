import importlib.util
from pathlib import Path
import socket
from types import ModuleType

from pymodbus.client import ModbusTcpClient
import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--device-ip", default=None, help="Device IP address")
    parser.addoption(
        "--device-port", default=502, type=int, help="Modbus TCP port (default 502)"
    )
    parser.addoption(
        "--ctl-file",
        default=None,
        help="Control file of a test-dynamic.host.yaml binary ($ESPHOME_TEST_CTL it was "
        "started with); lets tests change sensor values at runtime",
    )
    parser.addoption(
        "--allow-writes",
        action="store_true",
        default=False,
        help="Run tests that send power-limit commands. On a device with write-back "
        "configured these reach the real inverter.",
    )


@pytest.fixture(scope="module")
def client(request: pytest.FixtureRequest) -> ModbusTcpClient:
    ip = request.config.getoption("--device-ip")
    if ip is None:
        pytest.skip("--device-ip not provided")
    port = request.config.getoption("--device-port")
    c = ModbusTcpClient(ip, port=port)
    if not c.connect():
        pytest.fail(f"Cannot connect to Modbus TCP server at {ip}:{port}")
    yield c
    c.close()


class SensorControl:
    """Writes the control file read by tests/components/common/test_ctl.h."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.values: dict[str, str] = {}

    def set(self, **values: float | str) -> None:
        """Override sensors: a number, "nan", or "skip" (stop publishing)."""
        self.values.update({k: str(v) for k, v in values.items()})
        self._write()

    def reset(self) -> None:
        self.values.clear()
        self._write()

    def _write(self) -> None:
        tmp = self.path.with_name(self.path.name + ".tmp")
        tmp.write_text("".join(f"{k}={v}\n" for k, v in self.values.items()))
        tmp.replace(self.path)  # atomic: the device never sees a half-written file


@pytest.fixture
def ctl(request: pytest.FixtureRequest):
    """Sensor control for a test-dynamic host build; reset to defaults afterwards."""
    path = request.config.getoption("--ctl-file")
    if path is None:
        pytest.skip("--ctl-file not provided")
    control = SensorControl(Path(path))
    control.reset()
    yield control
    control.reset()


@pytest.fixture
def raw_request(client: ModbusTcpClient, request: pytest.FixtureRequest):
    """Send a raw Modbus TCP frame on a fresh connection; returns the reply bytes.

    For malformed requests that pymodbus refuses to build. Returns b"" when the
    server closes the connection without replying.
    """
    ip = request.config.getoption("--device-ip")
    port = request.config.getoption("--device-port")

    def send(frame: bytes, timeout: float = 2.0) -> bytes:
        with socket.create_connection((ip, port), timeout=timeout) as sock:
            sock.sendall(frame)
            try:
                return sock.recv(512)
            except (ConnectionResetError, TimeoutError):
                return b""

    return send


@pytest.fixture
def allow_writes(request: pytest.FixtureRequest) -> None:
    if not request.config.getoption("--allow-writes"):
        pytest.skip("sends power-limit commands; pass --allow-writes to run")


def _load_component(name: str) -> ModuleType:
    """Import components/<name>/__init__.py without needing it on sys.path."""
    spec = importlib.util.spec_from_file_location(
        f"components.{name}", REPO_ROOT / "components" / name / "__init__.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def load_component():
    return _load_component
