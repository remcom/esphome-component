# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A monorepo of ESPHome external components. Each component lives in `components/<name>/` and is consumed via `external_components` pointing at this repository. User-facing documentation per component is in `docs/<name>.md`; keep it in sync when changing a component's config schema or behaviour.

| Component | Summary |
|-----------|---------|
| `grid_meter` | Emulates a Carlo Gavazzi EM24 Ethernet meter over Modbus TCP so a Victron Cerbo GX can use a DSMR P1 smart meter as a grid meter |
| `sunspec` | SunSpec Modbus TCP server for a solar inverter, with power-limit write-back over RS485 Modbus |

## Repository Structure

```
components/
  grid_meter/        # __init__.py, grid_meter.h, grid_meter.cpp
  sunspec/           # __init__.py, sunspec.h, sunspec.cpp
docs/
  grid_meter.md
  sunspec.md
tests/
  conftest.py        # shared --device-ip / --device-port options + Modbus `client` fixture
  grid_meter/        # test_schema.py, test_grid_meter.py
  sunspec/           # test_schema.py, test_sunspec.py
```

### Adding a new component

1. `components/<name>/` with `__init__.py`, `<name>.h`, `<name>.cpp`
2. `docs/<name>.md` with installation, configuration and limitations
3. `tests/<name>/test_schema.py` (no hardware) and, where useful, an integration test using the shared `client` fixture
4. Add a row to the component tables in `README.md` and this file

## Running Tests

```bash
pip install -r requirements.txt
pytest -k schema -v                                             # all schema tests, no hardware
pytest tests/<component>/ --device-ip=<ESP32_IP> -v             # integration tests against a flashed device
```

pytest runs with `--import-mode=importlib` (see `pyproject.toml`) because each component has its own `test_schema.py`.

**Schema test caveat:** schema tests that run a full `CONFIG_SCHEMA` fail if ESPHome `CORE` is not initialised with an ESP32 platform context, because `cv.only_on_esp32` is part of the schema.

## Licensing

Same dual license as ESPHome (see `LICENSE`): C/C++ files (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`, `.ino`) are GPLv3, Python and everything else is MIT.

## ESPHome Component Standards

Follow https://github.com/esphome/esphome/blob/dev/.ai/instructions.md. Key rules for all components:

### Python (`__init__.py`)

- Import constants from `esphome.const` where they exist; define local `CONF_*` strings only when no matching constant exists.
- Declare `DEPENDENCIES = [...]` for always-required components. Optional components (used only when configured) must NOT be listed — that would force-load them unconditionally.
- Add `cv.only_on_esp32` to `CONFIG_SCHEMA` for any component using POSIX socket APIs.
- Use `cv.All(schema, validator_fn)` for cross-field validation, `cv.Inclusive` for all-or-none groups.
- Add `cv.Length(max=N)` validators for strings that map to fixed-size registers.
- Required + invariant fields → constructor parameters via `cg.new_Pvariable()`, not setters.

### C++

- **`this->` prefix**: All class member access must use `this->member_`.
- **Namespace**: C++17 nested style — `namespace esphome::<name> {` / `}  // namespace esphome::<name>`.
- **`dump_config()`**: Every component must implement `void dump_config() override`.
- **Constants**: `static constexpr` / `constexpr` over `static const`.
- **Casts**: `static_cast<>` / `reinterpret_cast<>` — no C-style casts.
- **No VLAs**: Use fixed-size arrays sized to the known maximum.
- **TAG in `.cpp`**: `static const char *const TAG` belongs in the `.cpp` file only.
- **POSIX headers in `.cpp`**: `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` etc. belong in the `.cpp`, not the header.
- **Check syscall return values**: `fcntl()`, `send()` and similar must have their return values checked and errors logged.
- **Blank line between function definitions.**

### Framework requirement

Both current components are ESP32 only (POSIX sockets: `socket()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()`, `fcntl()`). They work with both ESP-IDF and Arduino frameworks on ESP32 (Arduino on ESP32 runs on top of ESP-IDF). Not compatible with ESP8266 or RP2040.

---

## Component: `grid_meter`

Emulates a Carlo Gavazzi EM24 Ethernet energy meter over Modbus TCP (port 502), so a Victron Cerbo GX can treat a Dutch DSMR P1 smart meter connected to an ESP32 as a grid energy meter.

### How it works

`GridMeterComponent` (a standard ESPHome `Component`) runs a non-blocking Modbus TCP server in `loop()`:
1. Sensor state callbacks set `registers_dirty_`; when set, `refresh_sensors_()` rebuilds `registers_[]` — a dense `uint16_t[80]` array covering EM24 addresses `0x0000–0x004F` (event-driven, not every loop)
2. A stale-data watchdog suspends Modbus service when no valid power reading arrives within `data_timeout` (default 30 s), so the Cerbo marks the meter offline instead of using frozen values
3. Accepts/reads/processes client connections (up to 2 simultaneous, `TCP_NODELAY`)

Sensors: eight required references (L1 + energy totals, constructor parameters) plus optional per-phase L2/L3 groups (`set_phase_sensors()`, enforced all-or-none via `cv.Inclusive`). Configuring L2/L3 switches `PhaseConfig` (0x1002) from 3 (1P) to 0 (3P.n).

Sparse EM24 registers outside the dense range (`0x0302`, `0x0304`, `0x1002`, `0xa000`, `0xa100`, serial at `0x5000–0x5006`) are handled in `get_register_()`.

### Key protocol details

- **Why EM24, not ET112:** Victron's `dbus-modbus-client` only recognises EM24 model IDs (1648–1653) over TCP; ET112 IDs only work via RS485.
- **Word order:** All int32 values use `Reg_s32l` (little-endian word order — low word at lower address). The pymodbus client in tests reconstructs them big-endian — see `read_int32()` in `tests/grid_meter/test_grid_meter.py`.
- **FC06/FC16 writes** are accepted as no-ops (required for Cerbo init sequence).
- **hold-on-NaN:** Voltage, current, and energy counters use shadow registers that retain the last valid value when the sensor reports NaN. Power is zeroed on NaN instead (instantaneous value; the watchdog handles prolonged loss).
- **Power sign:** net power = `power_import - power_export`; positive = importing from grid.
- **Current:** always stored as positive magnitude regardless of power direction.

### ESPHome integration

`__init__.py` declares the eight L1/energy sensor keys as `cv.Required` and passes them (plus `port`) as constructor arguments via `cg.new_Pvariable()`. Optional keys: per-phase L2/L3 sensor groups, `serial_number` (≤14 chars, Reg_text at 0x5000), `data_timeout` (0 disables the watchdog). `cv.only_on_esp32` is enforced via `cv.All()` in the schema.

---

## Component: `sunspec`

Runs a SunSpec Modbus TCP server (port 502) on an ESP32. It reads live inverter data from existing ESPHome sensors and serves it to SunSpec-compatible energy managers (Victron, SolarEdge, Fronius, etc.). It also accepts power limit commands and relays them back to the inverter over RS485 Modbus. Built for a Solis single-phase inverter bridged via an ESP32 running ESP-IDF.

### SunSpec register layout

| Range      | Model | Content                                               |
|------------|-------|-------------------------------------------------------|
| 40000–40069 | 1    | Common block (manufacturer, model, serial, version)   |
| 40070–40121 | 101/103 | Inverter, single or three phase per `phases` config (power, voltage, current, freq, temp, energy, DC values, state) |
| 40122–40149 | 120  | Nameplate (DER type, rated power)                     |
| 40150–40175 | 123  | Controls (WMaxLimPct @ 40155 with SF=-2, WMaxLim_Ena @ 40159) |
| 40176–40177 | —    | End model (ID 0xFFFF, length 0)                       |

Writes (FC06/FC16) are accepted only in the Model 123 control window 40152–40172; scale factors (40173–40175) are read-only. Requests for any unit ID are served (SunSpec clients like Victron probe various unit IDs during discovery — rejecting mismatches broke Cerbo GX detection); the configured `address` is only reported in the Model 1 DA register. Sensors that stop updating for `stale_timeout` (default 5 min) are reported as "not implemented" (0x8000 for int16, 0xFFFF for uint16) and the inverter state falls back to Off.

Register updates are event-driven: `add_on_state_callback` on each sensor patches only that sensor's register slot(s) via `update_slot_()`; `loop()` only handles TCP traffic and the once-per-second stale scan. The component supports `MULTI_CONF` (multiple instances, each on a unique port — enforced by `FINAL_VALIDATE_SCHEMA`).

String fields use big-endian uint16 register pairs (2 chars per register):
- Manufacturer, model, serial: 16 registers = 32 chars max
- Version, options: 8 registers = 16 chars max

### Power limit write-back

`WMaxLimPct` is stored with scale factor -2 (raw 10000 = 100.00 %). Three mechanisms; the `on_power_limit` trigger always fires, plus at most one write-back path:

**Via number entity (recommended):**
- `WMaxLim_Ena = 1`: sets number entity to the limit percentage (0–100)
- `WMaxLim_Ena = 0`: sets number entity to `110` (maps to unlimited)

**Via direct Modbus register write (legacy; `modbus_controller_id` and `power_limit_register` must appear together):**
- `WMaxLim_Ena = 1`: writes the limit percentage (rounded to whole %) to `power_limit_register`
- `WMaxLim_Ena = 0`: writes `100` to restore full power

**Via `on_power_limit` automation:** fires with `level` (float, 0–100 %) and `enabled` (bool).
