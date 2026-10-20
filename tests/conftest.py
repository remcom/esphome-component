import pytest
from pymodbus.client import ModbusTcpClient


def pytest_addoption(parser):
    parser.addoption("--device-ip", default=None, help="ESP32 device IP address")
    parser.addoption("--device-port", default=502, type=int, help="Modbus TCP port (default 502)")


@pytest.fixture(scope="module")
def client(request):
    ip = request.config.getoption("--device-ip")
    if ip is None:
        pytest.skip("--device-ip not provided")
    c = ModbusTcpClient(ip, port=request.config.getoption("--device-port"))
    c.connect()
    yield c
    c.close()
