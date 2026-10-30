from collections.abc import Callable
import importlib.util
from pathlib import Path
import socket
import sys
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
        "--allow-writes",
        action="store_true",
        default=False,
        help="Run tests that send power-limit commands. On a device with write-back "
        "configured these reach the real inverter.",
    )
    parser.addoption(
        "--aqara-fp2-build-dir",
        default=None,
        help="ESPHome build directory holding the aqara_fp2 host builds "
        "(tests/components/aqara_fp2/.esphome/build) for the aqara_fp2 integration tests",
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


@pytest.fixture
def raw_request(
    client: ModbusTcpClient, request: pytest.FixtureRequest
) -> Callable[..., bytes]:
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
    """Import components/<name>/__init__.py without needing it on sys.path.

    The package is registered in sys.modules, so its platform modules (which use relative
    imports) can then be loaded with importlib.import_module(f"components.{name}.<platform>").
    """
    directory = REPO_ROOT / "components" / name
    spec = importlib.util.spec_from_file_location(
        f"components.{name}",
        directory / "__init__.py",
        submodule_search_locations=[str(directory)],
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def load_component() -> Callable[[str], ModuleType]:
    return _load_component
