# ESPHome components

A collection of [ESPHome](https://esphome.io) external components, all served from this single repository.

| Component | What it does | Docs |
|-----------|--------------|------|
| `aqara_fp2` | Runs ESPHome on an Aqara FP2 mmWave presence sensor: presence, motion, zones and live target positions | [docs/aqara_fp2.md](docs/aqara_fp2.md) |
| `grid_meter` | Serves DSMR P1 smart meter data as a Carlo Gavazzi EM24 over Modbus TCP, so a Victron Cerbo GX can use it as a grid meter | [docs/grid_meter.md](docs/grid_meter.md) |
| `sunspec` | Runs a SunSpec Modbus TCP server for a solar inverter (Models 1, 101/103, 120, 123) with power-limit write-back | [docs/sunspec.md](docs/sunspec.md) |

These components were previously published as separate repositories:
[victron-grid-meter-esphome](https://github.com/remcom/victron-grid-meter-esphome) and
[sunspec-esphome](https://github.com/remcom/sunspec-esphome).

## Installation

Reference this repository from `external_components` and list only the components you need:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    refresh: 1d
    components:
      - aqara_fp2
      - grid_meter
      - sunspec
```

Then configure each component as described in its documentation.

### Migrating from the old repositories

Only the `url` (and for `sunspec`, the `ref`) changes; component names and configuration keys are the same:

```yaml
# before
url: https://github.com/remcom/sunspec-esphome
ref: master
# after
url: https://github.com/remcom/esphome-component
ref: main
```

## Repository layout

```
components/<name>/         # ESPHome component: __init__.py (schema + codegen), .h/.cpp (runtime)
docs/<name>.md             # User documentation per component
tests/<name>/              # Schema tests (no hardware) and integration tests (live device or host build)
tests/components/<name>/   # Compile-test configurations per platform
tests/conftest.py          # Shared pytest options and Modbus TCP fixtures
```

## Requirements

ESPHome 2026.4.0 or newer. `grid_meter` and `sunspec` use ESPHome's `socket` component and are
compile-tested on ESP32 (ESP-IDF and Arduino), ESP8266, RP2040 (Pico W) and the host platform.
`aqara_fp2` targets the FP2's ESP32 and is compile-tested on ESP32 (ESP-IDF and Arduino) and host.

## Running tests

```bash
pip install -r requirements.txt

# Lint and format (ruff, clang-format; same versions as ESPHome)
pre-commit run --all-files

# Static analysis with ESPHome's .clang-tidy (builds the host test configs for a compile database)
script/clang-tidy.sh

# Schema tests for all components (no hardware required)
pytest -k schema -v

# Compile a test configuration
esphome compile tests/components/<component>/test.esp32-idf.yaml

# Integration tests against a flashed device
pytest tests/<component>/ --device-ip=<DEVICE_IP> [--device-port=502] -v
```

The integration tests can also run against a host build instead of hardware; see the component docs. CI
(`.github/workflows/ci.yml`) runs all of the above, including the integration tests against host builds.

## License

Licensed the same way as ESPHome itself: the C++ runtime code (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`, `.ino`)
is published under the GPLv3, and the Python code and everything else under the MIT license.
See [LICENSE](LICENSE) for details.
