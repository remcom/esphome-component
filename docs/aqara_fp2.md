# aqara_fp2 — Aqara FP2 presence sensor

Run ESPHome on an Aqara FP2 mmWave presence sensor. The component talks to the FP2's radar coprocessor
over its internal UART, configures it at boot (mounting position, sensitivity, detection grids and
zones) and publishes presence, motion, per-zone occupancy and live target positions. The FP2's
on-board accelerometer is supported too, so the radar can be told how the device is mounted.

> Work in progress: the radar protocol was reverse-engineered from the stock firmware. Expect rough
> edges, and keep a backup of the original flash before replacing the firmware.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    components:
      - aqara_fp2
```

## Configuration

The radar runs at 890000 baud, 8N1; the `uart` bus must have both TX and RX pins. At that speed
the default 256-byte receive buffer fills in 3 ms, so radar frames are lost whenever the main loop
stalls (an I2C timeout alone blocks it for 100 ms); give the bus a larger `rx_buffer_size`.

```yaml
uart:
  id: radar_uart
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 890000
  rx_buffer_size: 4096

i2c:
  id: internal_i2c
  scl: GPIO18
  sda: GPIO19

aqara_fp2:
  uart_id: radar_uart
  reset_pin: GPIO13
  mounting_position: wall
  presence_sensitivity: medium
  accel:
    i2c_id: internal_i2c
  zones:
    - sensitivity: high
      grid: |-
        . . X X X . . . . . . . . .
        . . X X X . . . . . . . . .
        . . X X X . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .
        . . . . . . . . . . . . . .

binary_sensor:
  - platform: aqara_fp2
    zone_1:
      presence:
        name: Sofa presence
      motion:
        name: Sofa motion
```

The pin numbers above are placeholders; use the ones of your FP2 board.

### Options

| Key | Default | Description |
|-----|---------|-------------|
| `uart_id` | — | UART bus connected to the radar (890000 baud, TX and RX required) |
| `reset_pin` | — | Radar reset pin; pulsed low for 100 ms at boot |
| `mounting_position` | `wall` | `wall`, `left_corner` or `right_corner` |
| `left_right_reverse` | `false` | Swap left and right in the radar's coordinate system |
| `presence_sensitivity` | `medium` | Global presence sensitivity: `low`, `medium` or `high` |
| `interference_grid` | — | Grid of cells the radar ignores (fans, curtains, …) |
| `exit_grid` | — | Grid of entrance/exit cells |
| `edge_grid` | — | Grid marking the detection boundary |
| `zones` | — | Up to 30 detection zones, each with `grid` and `sensitivity` (default `medium`). Zone entities go on the `binary_sensor` platform as `zone_1`, `zone_2`, … in the order of this list |
| `motion_timeout` | `30s` | Turn the `motion` binary sensors off when no movement event arrived for this long, see [Presence and motion](#presence-and-motion); `0s` keeps them on until leave or empty |
| `radar_timeout` | `60s` | Mark the radar lost when it sends nothing for this long, see [Radar watchdog](#radar-watchdog); `0s` disables |
| `accel` | — | On-board accelerometer: `i2c_id`, `address` (default `0x27`). Sampled every 100 ms |

### Grids

A grid is 14 rows of 14 cells: `X` marks an active cell, `.` or `-` an inactive one; spaces are
ignored. The 14×14 area maps to rows 0–13 and columns 2–15 of the radar's native 20×16 map.

### Entities

All entities use `platform: aqara_fp2`. Accelerometer entities require the `accel:` block.

```yaml
binary_sensor:
  - platform: aqara_fp2
    presence:
      name: Presence
    motion:
      name: Motion
    has_target:
      name: Has target
    zone_1:             # zone_1 … zone_30, each with presence and motion
      presence:
        name: Sofa presence
    vibration:          # accelerometer
      name: Vibration

event:
  - platform: aqara_fp2
    movement:           # event types: enter, leave, approach, away
      name: Movement

sensor:
  - platform: aqara_fp2
    target_count:
      name: Target count
    radar_temperature:
      name: Radar temperature
    target_1:           # target_1 … target_5, each with x, y, z (mm) and speed (mm/s)
      x:
        name: Target 1 X
      y:
        name: Target 1 Y
    tilt_angle:         # accelerometer, degrees from vertical
      name: Tilt angle

text_sensor:
  - platform: aqara_fp2
    radar_version:
      name: Radar version
    orientation:        # accelerometer
      name: Orientation

switch:
  - platform: aqara_fp2
    location_report:
      name: Report targets

button:
  - platform: aqara_fp2
    restart_radar:      # pulses reset_pin and reconfigures the radar
      name: Restart radar
```

`restart_radar` resets the radar through `reset_pin` and sends the configuration again on its first
heartbeat; without a `reset_pin` it only reconfigures. Pressing it with the log open shows the
radar's start-up, which otherwise happens before a log client can connect.

### Presence and motion

`presence` and the zone `presence` sensors are the radar's own occupancy states. The radar has no
motion state: it reports movement events (someone enters, leaves, approaches or walks away). The
`movement` event entity fires on each of them. The `motion` binary sensor is derived from those
events: it turns on with every event except leave, and off on a leave event, when `presence`
clears, or when no event arrived for `motion_timeout`. Zone `motion` works the same way with the
radar's per-zone events, whose meanings are not decoded yet: any zone event counts as motion, and
it turns off when the zone's presence clears or after `motion_timeout`.

The event names follow the Aqara FP1, which numbers its events the same way; only enter (0),
leave (1), approach (6) and away (7) have been seen. Other values are logged at debug level.

Target positions, `target_count` and `has_target` come from location reports: while location
reporting is off they are unknown. Without a `location_report` switch it is always on; with one,
the switch's `restore_mode` (default `RESTORE_DEFAULT_ON`: on, unless it was switched off before
the restart) decides the state at boot. If the radar does not acknowledge a change, the switch goes
back to the last state the radar did acknowledge.

## How it works

The radar sends a heartbeat (its firmware version) once it is up. On the heartbeat the component
sends the configuration, one write at a time: each write waits for the radar's acknowledgement
(500 ms) and is sent at most 3 times. If a write is not acknowledged, the component sets a warning
and starts the configuration again on the next heartbeat instead of continuing with a
half-configured radar. Nothing is sent before the first heartbeat.

Reports from the radar are acknowledged and published. When the radar asks for the device
orientation and tilt, the component answers right away with the accelerometer's readings, or
"upright, 0°" when no `accel:` is configured.

### Radar watchdog

When the radar sends nothing for `radar_timeout`, the component sets a warning, marks the radar
entities unknown and sends the configuration again on the next heartbeat. A radar that resets on
its own is only reconfigured this way if it stays silent for at least `radar_timeout`; a faster
restart is not detected yet, because a restart heartbeat cannot be told apart from a regular one.

How often the radar sends its heartbeat while nothing happens in the room has not been confirmed
on hardware yet. If the warning shows up in an empty room although the radar works, raise
`radar_timeout` or set it to `0s`.

## Scope / Limitations

- Fall detection is not implemented.
- A radar that restarts faster than `radar_timeout` is not reconfigured (see above).
- Radar frames longer than 96 bytes (for example thermal data, which the stock start-up sequence
  enables) are checked and acknowledged but not processed.
- The accelerometer is sampled at a fixed 100 ms: the stock firmware's orientation and vibration
  logic counts samples and assumes that rate. A failed read skips that sample; the component only
  sets a warning after 10 failed reads in a row (1 s). ESP-IDF still logs each I2C timeout itself
  (`i2c.master: I2C bus is still busy but software timeout detected`).

## Running tests

```bash
pytest tests/aqara_fp2/test_schema.py -v                       # schema tests, no hardware
esphome compile tests/components/aqara_fp2/test.esp32-idf.yaml

# Integration tests: the host builds talk to a fake radar on a pseudo-terminal
esphome compile tests/components/aqara_fp2/test.host.yaml
esphome compile tests/components/aqara_fp2/test-location-off.host.yaml
pytest tests/aqara_fp2/test_aqara_fp2.py --aqara-fp2-build-dir=tests/components/aqara_fp2/.esphome/build -v
```

`test-accel.host.yaml` (accelerometer on host I2C) only validates on Linux; `script/clang-tidy.sh`
skips it on other systems.
