# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A monorepo of ESPHome external components. Each component lives in `components/<name>/` and is consumed via `external_components` pointing at this repository. Components are standalone: they must not depend on each other or on shared code in this repo. User-facing documentation per component is in `docs/<name>.md`; keep it in sync when changing a component's config schema or behaviour.

| Component | Summary |
|-----------|---------|
| `aqara_fp2` | Aqara FP2 mmWave presence sensor: radar over UART (presence, motion, zones, target positions) plus its I2C accelerometer |
| `grid_meter` | Emulates a Carlo Gavazzi EM24 Ethernet meter over Modbus TCP so a Victron Cerbo GX can use a DSMR P1 smart meter as a grid meter |
| `sunspec` | SunSpec Modbus TCP server for a solar inverter, with power-limit write-back over RS485 Modbus |

## Repository Structure

```
components/
  aqara_fp2/         # __init__.py, aqara_fp2.h/.cpp, fp2_accel.h/.cpp, entity platforms
  grid_meter/        # __init__.py, grid_meter.h, grid_meter.cpp
  sunspec/           # __init__.py, sunspec.h, sunspec.cpp
docs/
  aqara_fp2.md
  grid_meter.md
  sunspec.md
tests/
  conftest.py        # --device-ip / --device-port / --allow-writes, `client`, `raw_request`, `load_component` fixtures
  aqara_fp2/         # test_schema.py, test_aqara_fp2.py (fake radar on a pseudo-terminal)
  grid_meter/        # test_schema.py, test_grid_meter.py
  sunspec/           # test_schema.py, test_sunspec.py
  components/        # ESPHome compile-test configs (ESPHome's tests/components layout)
    common/          # per-platform base: esp32-idf, esp32-ard, esp8266-ard, rp2040-ard, host
    <name>/          # common.yaml + test[-variant].<platform>.yaml
script/clang-tidy.sh       # clang-tidy over the host builds of tests/components/*/test*.host.yaml
.github/workflows/ci.yml   # lint, clang-tidy, schema tests, compile matrix, host integration tests
```

### Adding a new component

1. `components/<name>/` with `__init__.py`, `<name>.h`, `<name>.cpp`
2. `docs/<name>.md` with installation, configuration and limitations
3. `tests/<name>/test_schema.py` (no hardware) and, where useful, an integration test using the shared `client` fixture
4. `tests/components/<name>/common.yaml` and a `test.<platform>.yaml` per platform (CI compiles every `tests/components/*/test*.yaml` automatically)
5. Add a row to the component tables in `README.md` and this file

## Running Tests

```bash
pip install -r requirements.txt
pre-commit run --all-files                                      # ruff + clang-format (ESPHome's versions)
script/clang-tidy.sh                                            # clang-tidy with ESPHome's .clang-tidy (uses host builds)
pytest -k schema -v                                             # all schema tests, no hardware
esphome compile tests/components/<name>/test.esp32-idf.yaml     # compile test
pytest tests/<component>/ --device-ip=<DEVICE_IP> -v            # integration tests against a flashed device
```

Integration tests can also run against host builds (`test.host.yaml` for grid_meter on port 5020, `test-number.host.yaml` for sunspec on port 5021) — see the docs of each component. The aqara_fp2 integration test starts its host builds itself (`--aqara-fp2-build-dir=tests/components/aqara_fp2/.esphome/build`; it needs `test.host.yaml` and `test-location-off.host.yaml` built) and plays the radar on a pseudo-terminal linked at `/tmp/aqara-fp2-radar`. sunspec tests that write power limits are skipped unless `--allow-writes` is passed (on real hardware they limit the inverter).

pytest runs with `--import-mode=importlib` (see `pyproject.toml`) because each component has its own `test_schema.py`; schema tests load a component with the `load_component` fixture.

Minimum ESPHome version is 2026.4.0 (`cv.require_esphome_version` in every schema; CI compiles the host configs on it). `requirements.txt` pins the version used for development and CI.

## Licensing

Same dual license as ESPHome (see `LICENSE`): C/C++ files (`.c`, `.cpp`, `.h`, `.hpp`, `.tcc`, `.ino`) are GPLv3, Python and everything else is MIT.

## ESPHome Component Standards

Follow https://github.com/esphome/esphome/blob/dev/AGENTS.md. Key rules for all components:

### Python (`__init__.py`)

- Metadata constants in alphabetical order: `AUTO_LOAD`, `CODEOWNERS = ["@remcom"]`, `DEPENDENCIES`, `DOMAIN = "<name>"`, `MULTI_CONF`.
- Import constants from `esphome.const` where they exist; define local `CONF_*` strings only when no matching constant exists.
- Declare `DEPENDENCIES = [...]` for always-required components. Optional components (used only when configured) must NOT be listed — that would force-load them unconditionally. Code using an optional component goes behind a `cg.add_define()` emitted only when it is configured.
- Use `cv.All(schema, validator_fn)` for cross-field validation, `cv.Inclusive` for all-or-none groups, `cv.has_at_most_one_key` for mutually exclusive keys.
- Strings packed into registers: ASCII only and `cv.Length(max=N)` matching the register count.
- Required + invariant fields → constructor parameters via `cg.new_Pvariable()`, not setters.
- Type-hint all signatures, test functions and fixtures included (`ConfigType` from `esphome.types`); use `if (x := config.get(KEY)) is not None:` for optional keys.
- Automations: `automation.validate_automation({})` + `automation.build_callback_automation(...)`, no `Trigger` subclass.

### C++

- **`this->` prefix**: All class member access must use `this->member_`.
- **Namespace**: C++17 nested style — `namespace esphome::<name> {` / `}  // namespace esphome::<name>`.
- **Naming**: protected/private members and methods end with `_`.
- **`dump_config()`**: Every component must implement `void dump_config() override`; don't check `is_failed()` there (core logs it).
- **Constants**: `static constexpr` / `constexpr` over `static const`.
- **Casts**: `static_cast<>` / `reinterpret_cast<>` — no C-style casts.
- **No VLAs**: Use fixed-size arrays sized to the known maximum.
- **TAG in `.cpp`**: `static const char *const TAG` belongs in the `.cpp` file only.
- **Sockets**: use ESPHome's `socket` component (`socket::socket_ip_loop_monitored()`, `accept_loop_monitored()`), never raw POSIX calls. When `ready()` returns true, read/accept until the call would block. Check return values of `setblocking()`, `setsockopt()`, `bind()`, `listen()`, `write()`. Never block the loop waiting for a send buffer.
- **Time**: use `App.get_loop_component_start_time()` in `loop()`, not `millis()`; `set_interval()` for periodic work ≥ 500 ms.
- **Failures**: `mark_failed(LOG_STR("reason"))` in `setup()` only, without an `ESP_LOGE` of the same reason; runtime problems use `status_set_warning()` / `status_clear_warning()`.
- **Callbacks**: `template<typename F> void add_on_x_callback(F &&cb)` — never `std::function` in the signature.
- **Blank line between function definitions.**
- Formatting is enforced by clang-format (`.clang-format`, ESPHome's) via pre-commit; `script/clang-tidy.sh` runs ESPHome's `.clang-tidy` (static class methods have no trailing `_`; round with `std::lround`, not `+ 0.5`).

### Platform support

`grid_meter` and `sunspec` use ESPHome's `socket` component, so they are not tied to ESP32. CI compile-tests them on ESP32 (ESP-IDF and Arduino), ESP8266, RP2040 (Pico W) and host. `aqara_fp2` targets the FP2's ESP32 and is compile-tested on ESP32 (ESP-IDF and Arduino) and host (radar only: host I2C is Linux-only, and the host UART cannot run 890000 baud, so the baud-rate check is skipped on host).

---

## Component: `grid_meter`

Emulates a Carlo Gavazzi EM24 Ethernet energy meter over Modbus TCP (port 502), so a Victron Cerbo GX can treat a Dutch DSMR P1 smart meter connected to an ESP as a grid energy meter.

### How it works

`GridMeterComponent` (a standard ESPHome `Component`) runs a non-blocking Modbus TCP server in `loop()`:
1. Sensor state callbacks set `registers_dirty_`; when set, `refresh_sensors_()` rebuilds `registers_[]` — a dense `uint16_t[80]` array covering EM24 addresses `0x0000–0x004F` (event-driven, not every loop)
2. A stale-data watchdog refuses Modbus service until the first valid power reading and whenever none arrives within `data_timeout` (default 30 s), so the Cerbo marks the meter offline instead of using placeholder or frozen values. While suspended the component has a warning status.
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

`__init__.py` declares the eight L1/energy sensor keys as `cv.Required` and passes them (plus `port`) as constructor arguments via `cg.new_Pvariable()`. Optional keys: per-phase L2/L3 sensor groups, `serial_number` (ASCII, ≤14 chars, Reg_text at 0x5000), `data_timeout` (0 disables the watchdog).

---

## Component: `sunspec`

Runs a SunSpec Modbus TCP server (port 502). It reads live inverter data from existing ESPHome sensors and serves it to SunSpec-compatible energy managers (Victron, SolarEdge, Fronius, etc.). It also accepts power limit commands and relays them back to the inverter over RS485 Modbus. Built for a Solis single-phase inverter bridged via an ESP32 running ESP-IDF.

### SunSpec register layout

| Range      | Model | Content                                               |
|------------|-------|-------------------------------------------------------|
| 40000–40069 | 1    | Common block (manufacturer, model, serial, version)   |
| 40070–40121 | 101/103 | Inverter, single or three phase per `phases` config (power, voltage, current, freq, temp, energy, DC values, state) |
| 40122–40149 | 120  | Nameplate (DER type, rated power)                     |
| 40150–40175 | 123  | Controls (WMaxLimPct @ 40155 with SF=-2, WMaxLimPct_RvrtTms @ 40157, WMaxLim_Ena @ 40159) |
| 40176–40177 | —    | End model (ID 0xFFFF, length 0)                       |

Writes (FC06/FC16) are accepted only in the Model 123 control window 40152–40172; scale factors (40173–40175) are read-only. Values are range-checked (`is_valid_control_value()`: WMaxLimPct ≤ 10000, enable flags 0/1) and an FC16 with any invalid value writes nothing (exception 0x03). Requests for any unit ID are served (SunSpec clients like Victron probe various unit IDs during discovery — rejecting mismatches broke Cerbo GX detection); the configured `address` is only reported in the Model 1 DA register. Sensors that stop updating for `stale_timeout` (default 5 min) are reported as "not implemented" (0x8000 for int16, 0xFFFF for uint16) and the inverter state falls back to Off.

Register updates are event-driven: `add_on_state_callback` on each sensor patches only that sensor's register slot(s) via `update_slot_()`; `loop()` only handles TCP traffic and the revert timer, and a 1 s `set_interval` runs the stale scan. The component supports `MULTI_CONF` (multiple instances, each on a unique port, also distinct from a `grid_meter` port — enforced by `FINAL_VALIDATE_SCHEMA`).

Power registers (W, DCW, WRtg) share one scale factor computed from `rated_power` in Python (`power_scale_factor()`: smallest SF in 0..2 keeping 1.5× rated power within int16).

String fields use big-endian uint16 register pairs (2 chars per register):
- Manufacturer, model, serial: 16 registers = 32 chars max
- Version, options: 8 registers = 16 chars max

### Power limit write-back

`WMaxLimPct` is stored with scale factor -2 (raw 10000 = 100.00 %). Three mechanisms; the `on_power_limit` trigger always fires, plus at most one write-back path (`cv.has_at_most_one_key`). Write-back code is compiled only when configured (`USE_SUNSPEC_POWER_LIMIT_NUMBER` / `USE_SUNSPEC_MODBUS_CONTROLLER` defines), so trigger-only configs need neither `number` nor `modbus_controller`.

Write-back runs only when the limit changes (`applied_limit_`); an unchanged limit is re-forwarded at most every 5 minutes (`LIMIT_REFRESH_MS`). `WMaxLimPct_RvrtTms` (seconds, 0/0xFFFF = off) disables the limit when no new limit command arrives in time; every limit write restarts it.

**Via number entity (recommended):**
- `WMaxLim_Ena = 1`: sets number entity to the limit percentage (0–100)
- `WMaxLim_Ena = 0`: sets number entity to `110` (maps to unlimited)

**Via direct Modbus register write (legacy; `modbus_controller_id` and `power_limit_register` must appear together):**
- `WMaxLim_Ena = 1`: writes the limit percentage (rounded to whole %) to `power_limit_register`
- `WMaxLim_Ena = 0`: writes `100` to restore full power
- On ESPHome ≥ 2026.9 the write goes through the `LimitWriter` `ModbusClientDevice` on the controller's bus (failed writes are re-sent on the next command); older versions use the deprecated `ModbusCommandItem`, which is removed in 2027.3.0.

**Via `on_power_limit` automation:** fires with `level` (float, 0–100 %) and `enabled` (bool).

---

## Component: `aqara_fp2`

Runs ESPHome on an Aqara FP2 mmWave presence sensor. `FP2Component` (`Component` + `UARTDevice`) speaks the radar coprocessor's framed UART protocol at 890000 baud; `FP2Accel` (`Component` + `I2CDevice`, address 0x27, sampled every 100 ms from a gated `loop()`) reads the on-board accelerometer. Ported from the `aqara-fp2` branch of the ESPHome fork, where the accelerometer was a separate `aqara_fp2_accel` component; here it is the optional `accel:` block, because components in this repo must not depend on each other.

### How it works

- Frames: `0x55 0x00 0x01 seq opcode len_h len_l hdr_chk payload crc16_l crc16_h` (CRC16/MODBUS over header + payload). Opcodes: RESPONSE 0x01, WRITE 0x02, ACK 0x03, READ 0x04, REPORT 0x05. Payload starts with a 16-bit attribute ID (`AttrId`) and a data-type tag.
- `ConfigState`: WAIT_HEARTBEAT → (heartbeat, `RADAR_SW_VERSION` report) → SENDING → DONE. The configuration is not queued: `build_config_command_(step)` generates step N (attributes, grids, map + sensitivity per zone, activation list, close/away per zone) when the previous write was ACKed. Nothing is sent before the first heartbeat.
- One WRITE in flight (`inflight_`), sent at most `MAX_SENDS` times (`ACK_TIMEOUT_MS` apart). A failed configuration write sets a warning and returns to WAIT_HEARTBEAT (never continue half-configured); a failed runtime write only sets a warning.
- The 8-slot queue holds ACKs and answers to radar queries (both inserted at the front) and runtime writes (location report switch). Queued commands go before configuration steps.
- Watchdog: no valid frame for `radar_timeout` → warning, entities unknown, WAIT_HEARTBEAT. The radar's idle heartbeat interval is not confirmed on hardware, and a heartbeat in DONE is ignored, so a radar restart shorter than `radar_timeout` goes unnoticed.
- Decoder: a gap of more than `RX_BYTE_TIMEOUT_MS` (50 ms) inside a frame resynchronizes. The CRC is computed while bytes arrive, so frames longer than `MAX_RX_PAYLOAD` are read to the end, CRC-checked and (reports) ACKed without being processed; a declared length above `MAX_RX_FRAME_PAYLOAD` (2048) is treated as a corrupt header and resynchronizes at once.
- Location report switch: published optimistically. `location_reporting_confirmed_` holds the last state the radar ACKed; a dropped or unacknowledged write reverts to it (`revert_location_reporting_()`), clearing location entities when that state is off. While location reporting is off, targets, `target_count` and `has_target` are unknown, also across (re)configuration; with it on, configuration starts them at 0 / OFF.
- Radar queries: `DEVICE_DIRECTION` (UINT8) / `ANGLE_SENSOR_DATA` (UINT16, as the stock firmware), answered from `FP2Accel`, or upright/0 without `accel:`. The radar asks for the tilt only at boot.
- Presence/motion: the radar sends no presence (0x0104) or motion (0x0103) reports until it has the boot exchange (tilt/orientation answers, configuration incl. `WORK_MODE` = 3); a lost exchange (the UART's default 256-byte RX buffer overflows during loop stalls) leaves presence silent while location reports still work. 0x0103 is a movement event (0 enter, 1 leave, 6 approach, 7 away), exposed on the `event` platform as `movement`; the `motion` binary sensors are derived (`MotionTimer`: on per event, off on leave / presence clear / `motion_timeout`). Zone motion (0x0115) values are undecoded event flags.
- Every report except location and heartbeat is logged at debug level with its raw value (formats are reverse-engineered; hansihe/AqaraPresenceSensorFP2ReverseEngineering has stock-firmware UART captures).
- Location reporting defaults on; with a `location_report` switch its restore mode (default `RESTORE_DEFAULT_ON`) decides the boot state.
- Enum values carry the enum name as prefix (`OP_CODE_ACK`, `ATTR_ID_ZONE_MAP`, …) per ESPHome's naming rule.

### ESPHome integration

- No `AUTO_LOAD`. Zone entities are on the `binary_sensor` platform as `zone_<n>` (n = position in `zones:`), checked against the hub's zone count in the platform's `FINAL_VALIDATE_SCHEMA`. The hub's `to_code` creates the zones before its first `await`, so the platform's index-based setters always find them.
- Numeric sensors go through the local `DedupSensor<T>` (core `Deduplicator<T>`), not `ld24xx::SensorWithDedup`, whose API changed between 2026.4 and 2026.9.
- Accelerometer code is behind `USE_AQARA_FP2_ACCEL` (emitted only when `accel:` is configured), so radar-only configs need no `i2c`.
- Accelerometer entities (`vibration`, `tilt_angle`, `orientation`) live on the `aqara_fp2` platforms; `require_accel_for()` resolves `aqara_fp2_accel_id` only when one of them is configured.
- Grids: 14×14 ASCII → 40-byte map, rows 0–13, columns 2–15 of the radar's 20×16 map (`parse_ascii_grid()`).
- `FINAL_VALIDATE_SCHEMA` requires TX+RX, 8N1 and 890000 baud (baud not enforced on host).
- `test-accel.host.yaml` starts with `# linux-only` (host I2C only validates on Linux, so `script/clang-tidy.sh` skips it elsewhere) and `# esphome>=2026.6.0` (host I2C is newer than the minimum version, so the min-version CI job skips it). The accelerometer itself works from 2026.4.0 on ESP32.
