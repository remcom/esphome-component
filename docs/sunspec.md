# sunspec — SunSpec Modbus TCP server

> Previously published as [remcom/sunspec-esphome](https://github.com/remcom/sunspec-esphome).

Custom ESPHome external component that runs a **SunSpec Modbus TCP server** (default port 502) directly on an ESP. It reads live inverter data from existing ESPHome sensors and serves it to any SunSpec-compatible energy manager (Victron, SolarEdge, Fronius, etc.). It also accepts power limit commands from SunSpec clients and relays them back to the inverter over RS485 Modbus.

Built for a **Solis single-phase inverter** bridged via an ESP32 (m5stack-atom, ESP-IDF), but the sensor IDs and power limit register are configurable via YAML.

## Features

- SunSpec Models 1 (Common), 101/103 (Single/Three-phase inverter), 120 (Nameplate), 123 (Controls)
- FC03/FC04 read registers, FC06/FC16 write — power limit (WMaxLimPct), enable (WMaxLim_Ena) and revert timeout (WMaxLimPct_RvrtTms)
- Power limit write-back to inverter over RS485 via `modbus_controller` or a `number` entity, forwarded only when the limit changes
- `on_power_limit` automation trigger for custom write-back logic
- Power scale factor chosen from `rated_power`, so inverters above ~21 kW fit the 16-bit power registers
- Optional DC-side sensors (power, voltage, current)
- Event-driven register updates — registers are patched when a sensor publishes, not polled
- Stale-data detection: sensors that stop updating are reported as "not implemented" and the inverter state falls back to *Off*
- Configurable port, unit address, and connection limit (up to 8 simultaneous clients)
- Multiple server instances per device (one per port)
- Requires ESPHome 2026.4.0 or newer. Compile-tested on ESP32 (ESP-IDF and Arduino), ESP8266 and RP2040 (Pico W); the server uses ESPHome's `socket` component, so other platforms it supports should work too

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    refresh: 1d
    components:
      - sunspec
```

## Configuration

```yaml
sunspec:
  # Server settings (all optional)
  port: 502
  address: 1               # reported in the Model 1 DA register (any unit ID is served)
  max_connections: 4       # 1-8 simultaneous TCP clients
  stale_timeout: 5min      # report sensors as unavailable after this; 0s disables

  # Device identification (shown to SunSpec clients)
  manufacturer: "Solis"
  model: "S6-GR1P3K-M"
  serial_number: "12345678"    # optional
  version: "1.0.0"             # optional, default "1.0"
  rated_power: 3000            # watts, 1 to 2000000

  # Sensor references (ESPHome sensor IDs)
  phases: 1                          # 1 (Model 101, default) or 3 (Model 103)
  ac_power: ac_power                 # required
  ac_voltage: ac_voltage             # required (phase A voltage for phases: 3)
  ac_frequency: ac_frequency         # required
  temperature: inverter_temp         # required
  ac_current: ac_current             # optional total current (derived from power/voltage for phases: 1 if omitted)
  energy_total: energy_total         # optional (kWh)
  dc_power: dc_power                 # optional
  dc_voltage: dc_voltage             # optional
  dc_current: dc_current             # optional

  # Three-phase only (phases: 3)
  # ac_current_phase_a: current_l1
  # ac_current_phase_b: current_l2
  # ac_current_phase_c: current_l3
  # ac_voltage_phase_b: voltage_l2
  # ac_voltage_phase_c: voltage_l3

  # Power limit write-back: choose option A or B (not both); option C combines with either
  # Option A: via a number entity (recommended)
  power_limit_number_id: power_limit # ESPHome number entity ID (0–100 = limit %, 110 = unlimited)

  # Option B: direct Modbus register write (legacy)
  # modbus_controller_id: modbus_master
  # power_limit_register: 3051       # Solis RS485 register for power limit

  # Option C: custom automation (works on its own, without modbus_controller or number)
  # on_power_limit:
  #   - logger.log:
  #       format: "Power limit %.1f%% (enabled: %d)"
  #       args: [level, enabled]
```

The `power_limit_number_id` approach routes the limit through an existing `modbus_controller` number entity. Define it alongside your other number entities:

```yaml
number:
  - platform: modbus_controller
    modbus_controller_id: modbus_master
    id: power_limit
    name: Limit output power
    register_type: holding
    address: 3051           # = 3052 - 1 (Solis)
    value_type: S_WORD
    unit_of_measurement: '%'
    entity_category: config
    icon: mdi:percent
    skip_updates: 10
    mode: box
    min_value: 0
    max_value: 110
    multiply: 100
```

### Options

| Key | Required | Description |
|-----|----------|-------------|
| `port` | no | TCP port to listen on (default `502`) |
| `address` | no | Modbus unit address reported in the Model 1 DA register, 1–247 (default `1`); requests for any unit ID are served |
| `max_connections` | no | Simultaneous TCP clients, 1–8 (default `4`) |
| `stale_timeout` | no | Mark sensor values "not implemented" after no update for this long (default `5min`, `0s` disables) |
| `manufacturer` | yes | Manufacturer string (ASCII, max 32 chars) |
| `model` | yes | Model string (ASCII, max 32 chars) |
| `serial_number` | no | Serial number string (ASCII, max 32 chars) |
| `version` | no | Firmware version string (ASCII, max 16 chars, default `1.0`) |
| `rated_power` | yes | Inverter rated power in watts (1–2000000). Also sets the scale factor of the power registers (W_SF, DCW_SF, WRtg_SF): 0 up to 21844 W, 1 up to 218446 W, 2 above |
| `phases` | no | `1` (Model 101, default) or `3` (Model 103) |
| `ac_power` | yes | ESPHome sensor ID for AC power (W) |
| `ac_voltage` | yes | ESPHome sensor ID for AC voltage (V); phase A voltage when `phases: 3` |
| `ac_frequency` | yes | ESPHome sensor ID for AC frequency (Hz) |
| `temperature` | yes | ESPHome sensor ID for inverter temperature (°C) |
| `ac_current` | no | ESPHome sensor ID for total AC current (A); derived from power/voltage for single-phase if omitted |
| `ac_current_phase_a/b/c` | no | Per-phase current sensors (`phases: 3` only) |
| `ac_voltage_phase_b/c` | no | Phase B/C voltage sensors (`phases: 3` only) |
| `energy_total` | no | ESPHome sensor ID for lifetime energy (kWh) |
| `dc_power` | no | ESPHome sensor ID for DC power (W) |
| `dc_voltage` | no | ESPHome sensor ID for DC voltage (V) |
| `dc_current` | no | ESPHome sensor ID for DC current (A) |
| `power_limit_number_id` | no | ESPHome `number` entity ID to route power limit through (recommended). Cannot be combined with `modbus_controller_id` |
| `modbus_controller_id` | no | ID of your `modbus_controller` component (required together with `power_limit_register`) |
| `power_limit_register` | no | RS485 register address (0–65535) for direct power limit write (legacy; required together with `modbus_controller_id`) |
| `on_power_limit` | no | Automation triggered on limit commands; provides `level` (float, %) and `enabled` (bool) |

## SunSpec Register Map

| Range | Model | Content |
|-------|-------|---------|
| 40000–40069 | 1 | Common block (manufacturer, model, serial, version, unit address) |
| 40070–40121 | 101/103 | Inverter (power, voltage, current, frequency, temperature, energy, DC values, state) |
| 40122–40149 | 120 | Nameplate (DER type, rated power) |
| 40150–40175 | 123 | Controls (WMaxLimPct @ 40155, WMaxLim_Ena @ 40159) |
| 40176–40177 | — | End model (ID 0xFFFF, length 0) |

## Power Limiting

Write `WMaxLimPct` (register 40155, scale factor -2, so `10000` = 100.00 %) and `WMaxLim_Ena` (register 40159, `1` = enabled). Writes are accepted in the Model 123 control window (40152–40172); scale factors are read-only. Out-of-range values are rejected with Modbus exception 0x03 and not stored: `WMaxLimPct` above `10000`, or an enable flag (`Conn`, `WMaxLim_Ena`, `OutPFSet_Ena`, `VArPct_Ena`) other than 0/1. An FC16 write with any invalid value changes nothing.

**Revert timeout:** when `WMaxLimPct_RvrtTms` (register 40157, seconds) is set to a value other than `0` or `0xFFFF` while the limit is enabled, the limit is disabled again (`WMaxLim_Ena = 0`, full power) if no new limit command arrives within that time. Every write to `WMaxLimPct`, `WMaxLim_Ena` or `WMaxLimPct_RvrtTms` restarts the timer. This keeps the inverter from staying limited when the energy manager disappears.

**Change detection:** energy managers re-send the same limit every few seconds. The write-back (number entity or register write) only runs when the limit actually changes; an unchanged limit is forwarded again at most once every 5 minutes, in case the inverter missed a write. With `modbus_controller` write-back on ESPHome 2026.9 or newer, a write that fails or gets no response is retried on the next limit command. This keeps traffic down on the RS485 bus and avoids wearing out inverters that store the limit in EEPROM.

**Via number entity (`power_limit_number_id`, recommended):**
- When `WMaxLim_Ena = 1`: sets the number entity to the limit percentage (0–100)
- When `WMaxLim_Ena = 0`: sets the number entity to `110` (maps to "unlimited" / full power)
- The number entity's own `multiply` and write logic handles the actual Modbus write

**Via direct register write (`power_limit_register`, legacy):**
- When `WMaxLim_Ena = 1`: queues a write of the limit percentage (rounded to whole %) to `power_limit_register`
- When `WMaxLim_Ena = 0`: queues a write of `100` (full power restore)
- Writes are asynchronous — executed on the Modbus controller's next poll cycle

**Via `on_power_limit` automation:** fires on every limit command (including unchanged repeats and revert-timeout expiry) with `level` (0–100, float) and `enabled` (bool), in addition to any write-back configured above.

On ESP32 reboot, `WMaxLim_Ena` defaults to `0` and `WMaxLimPct` defaults to `10000` (100.00 %). The inverter's own power limit state is not read back on startup — re-apply any desired limit after reboot.

## Scope / Limitations

- No persistent energy counter across reboots
- Only `WMaxLimPct_RvrtTms` is acted upon; the other Model 123 window/ramp timers (`*_WinTms`, `*_RmpTms`) are stored but ignored
- No phase-to-phase voltages (PPVphAB/BC/CA report "not implemented")
- No authentication — Modbus TCP is unauthenticated by design; run it on a trusted network segment
- Multiple `sunspec:` instances are supported, but each needs a unique `port`, which must also differ from a `grid_meter` on the same device

## Running tests

From the repository root, after `pip install -r requirements.txt`.

Schema tests (no hardware required):

```bash
pytest tests/sunspec/test_schema.py -v
```

Integration tests run against a flashed device. Tests that write power-limit registers are skipped unless you pass `--allow-writes`; on a device with write-back configured they briefly limit the real inverter (to 50–80 %):

```bash
pytest tests/sunspec/test_sunspec.py --device-ip=<DEVICE_IP> -v [--allow-writes]
```

…or against a host build of the test configuration (fixed sensor values, template number for write-back, port 5021):

```bash
esphome compile tests/components/sunspec/test-number.host.yaml
tests/components/sunspec/.esphome/build/sunspec-number-host/.pioenvs/sunspec-number-host/program &
pytest tests/sunspec/test_sunspec.py --device-ip=127.0.0.1 --device-port=5021 --allow-writes -v
```

Behaviour that needs changing inputs is tested against `test-dynamic.host.yaml` (port 5023), whose sensors read their values from a control file (`$ESPHOME_TEST_CTL`, see `tests/components/common/test_ctl.h`):

```bash
esphome compile tests/components/sunspec/test-dynamic.host.yaml
ESPHOME_TEST_CTL=/tmp/sunspec.ctl tests/components/sunspec/.esphome/build/sunspec-dynamic-host/.pioenvs/sunspec-dynamic-host/program &
pytest tests/sunspec/test_dynamic.py --device-ip=127.0.0.1 --device-port=5023 --ctl-file=/tmp/sunspec.ctl -v
```
