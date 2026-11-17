# thermia_itec_eco — Thermia iTec Eco heat pump

Monitor a Thermia iTec Eco heat pump (Danfoss DHP-AQ controller) locally, without Thermia Online,
from the heat pump's internal RS485 bus. The component listens to the traffic between the display
panel and the heat pump's boards and decodes temperatures, compressor and outdoor unit data, the
controller's outputs and the heating settings.

It can also act as the Thermia **Modbus room sensor** (086U9563) on the bus. The heat pump then
uses a room temperature from ESPHome (any sensor, for example one from Home Assistant) for room
compensation, and you can change the room setpoint and start a hot water top-up from Home
Assistant.

> The bus protocol is reverse-engineered, not documented by Thermia. The register map comes from
> community research: [ryckema/Thermia_itec](https://github.com/ryckema/Thermia_itec) (iTec XTR M
> and the iTec Eco profile) and [piotr-romanowski/thermia-bus-sniffer](https://github.com/piotr-romanowski/thermia-bus-sniffer)
> (iTec Eco 8, display firmware 2.3.0), with discussion in the
> [Home Assistant community thread](https://community.home-assistant.io/t/local-thermia-itec-integration-with-esphome-home-assistant/1024113).
> Values marked as unconfirmed below were observed but not cross-checked against the display.
> Use at your own risk; transmitting is off unless you configure it.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    components:
      - thermia_itec_eco
```

Requires ESPHome 2026.10.0 or newer.

### Hardware

An ESP32 with an RS485 transceiver, for example the Waveshare ESP32-S3-RS485-CAN (RX GPIO18,
TX GPIO17, transceiver direction GPIO21). Connect it to the free RJ45 port on the heat pump's
controller board, next to the one the display uses (ports 122/123):

| RJ45 pins | Signal | Connect to |
|-----------|--------|------------|
| 1 + 2 | RS485 A | A |
| 3 + 4 | RS485 B | B |
| 5 + 6 | Ground / reference | GND (optional, recommended) |
| 7 + 8 | +12 V | **do not connect** |

Measure before connecting: pin 7 should be about +12 V against pin 5. Power the ESP separately
(USB). Compile-tested on ESP32 (ESP-IDF and Arduino) and host.

## Configuration

The bus runs Modbus RTU at 9600 baud, 8E1. Listening needs only an RX pin; the room sensor
emulation also needs TX and, for a transceiver with a direction pin, the UART's `flow_control_pin`
(ESP32: the UART then drives DE/RE in hardware RS485 mode).

```yaml
uart:
  id: heat_pump_bus
  rx_pin: GPIO18
  tx_pin: GPIO17
  flow_control_pin: GPIO21
  baud_rate: 9600
  parity: EVEN

thermia_itec_eco:
  uart_id: heat_pump_bus
  # Optional: act as the Modbus room sensor
  room_sensor:
    temperature: living_room_temperature

sensor:
  - platform: homeassistant
    id: living_room_temperature
    entity_id: sensor.living_room_temperature

  - platform: thermia_itec_eco
    supply_temperature:
      name: Supply temperature
    outdoor_temperature:
      name: Outdoor temperature
    hot_water_temperature:
      name: Hot water temperature
    room_setpoint:
      name: Room setpoint
    compressor_frequency:
      name: Compressor frequency

binary_sensor:
  - platform: thermia_itec_eco
    compressor:
      name: Compressor
    defrost:
      name: Defrost
    hot_water_top_up:
      name: Hot water top-up

text_sensor:
  - platform: thermia_itec_eco
    operating_mode:
      name: Operating mode

# Controls (need room_sensor)
switch:
  - platform: thermia_itec_eco
    room_sensor:
      name: Room sensor emulation

number:
  - platform: thermia_itec_eco
    room_setpoint:
      name: Room setpoint request

button:
  - platform: thermia_itec_eco
    hot_water_top_up:
      name: Start hot water top-up
```

### Options

| Key | Default | Description |
|-----|---------|-------------|
| `uart_id` | — | UART bus, 9600 baud, 8E1. |
| `bus_timeout` | `30s` | Without a valid frame for this long the component sets a warning and its entities become unknown. `0s` disables it. |
| `room_sensor` | — | Emulate the Modbus room sensor (see below). |
| `room_sensor.temperature` | required | ID of the sensor that provides the room temperature (°C). |
| `room_sensor.timeout` | `1h` | Stop answering when `temperature` has not updated for this long, so the heat pump falls back to its heating curve instead of using a stale value. `0s` disables it. |

Each platform entry may set `thermia_itec_eco_id` when there is more than one hub.

### Entities

**Sensors** (`sensor` platform)

| Key | Unit | Source | Notes |
|-----|------|--------|-------|
| `supply_temperature` | °C | controller 0x02 | whole degrees |
| `outdoor_temperature` | °C | controller 0x02 | whole degrees |
| `hot_water_top_temperature` | °C | controller 0x02 | upper tank probe |
| `hot_water_temperature` | °C | controller 0x02 | |
| `room_temperature` | °C | room sensor 0x0A | from a real room sensor, or the value the emulation sends |
| `room_setpoint` | °C | display → room sensor | the controller's room setpoint |
| `target_supply_temperature` | °C | display → outdoor unit | 0 while there is no demand |
| `condenser_inlet_temperature` | °C | outdoor unit 0x1E | unconfirmed on the Eco |
| `condenser_outlet_temperature` | °C | outdoor unit 0x1E | unconfirmed on the Eco |
| `high_pressure` | bar | outdoor unit 0x1E | unconfirmed on the Eco; unknown while the compressor is idle |
| `compressor_frequency` | Hz | outdoor unit 0x1E | |
| `compressor_current` | A | outdoor unit 0x1E | |
| `outdoor_fan_speed` | rpm | outdoor unit 0x1E | |
| `expansion_valve_position` | steps | outdoor unit 0x1E | |
| `circulation_pump_speed` | % | display → controller | 5 % means stopped |
| `heating_curve`, `heating_curve_min`, `heating_curve_max` | –, °C, °C | settings page | see below |
| `curve_correction_plus_5`, `curve_correction_0`, `curve_correction_minus_5` | K | settings page | curve offsets at +5 / 0 / −5 °C outside |
| `heating_stop_temperature`, `setback_temperature`, `room_factor` | °C, °C, – | settings page | |

The settings sensors come from the HEATING page the display writes to slave 0x0F. Without a
Thermia Online module it only does so when a setting is changed on the display, so these sensors
stay unknown until then (and after every reboot of the ESP).

**Binary sensors** (`binary_sensor` platform): `compressor`, `defrost`, `outdoor_fan`,
`circulation_pump` (water flow), `hot_water_valve` (reversing valve on hot water),
`immersion_heater_1`, `immersion_heater_2`, `external_aux_heater`, `hot_water_top_up` (a top-up
runs), `high_power` (setting, from the settings page).

**Text sensor** (`text_sensor` platform): `operating_mode` — what the display asks the outdoor
unit for: `Heating`, `Hot water`, `Cooling` (or `Unknown`). It keeps the last mode while the
compressor is idle.

**Controls** (need `room_sensor`):

| Platform | Key | Description |
|----------|-----|-------------|
| `switch` | `room_sensor` | Turns the emulation on or off. Starts **off** (`restore_mode` `RESTORE_DEFAULT_OFF`): nothing is transmitted until you turn it on. Without this switch the emulation is always on. |
| `number` | `room_setpoint` | 10–30 °C. Shows the controller's room setpoint; setting it sends a setpoint request. |
| `button` | `hot_water_top_up` | Starts a hot water top-up (the immersion heater heats the tank, like TOP-UP on the display). |

## How it works

The display panel is the bus master and polls every slave about every 4 seconds:

| Slave | Role | What the component reads |
|-------|------|--------------------------|
| 0x02 | indoor I/O board | FC23: temperatures (reply), outputs and pump speed (request) |
| 0x0A | room sensor slot | FC23: room temperature (reply), room setpoint and top-up state (request) |
| 0x0F | settings / online module | FC16: settings pages |
| 0x1E | outdoor unit | FC04: refrigerant circuit (reply); FC16: target and mode (request) |

Replies carry no register address, so each reply is decoded against the display's preceding
request. Frames are found by their CRC, so noise and partial frames are skipped.

### Room sensor emulation

With `room_sensor` configured and the switch on, the component answers the display's poll of slot
0x0A like a Thermia Modbus room sensor: `0A 17 06 <room temperature ×10> <setpoint request>
<top-up request> <CRC>`. The display detects the sensor by itself and uses the temperature for
room compensation (the room factor setting decides how much).

- **Setpoint request:** like turning the dial on a real room sensor. The request is repeated until
  the display pushes the new setpoint back (at most 5 replies, about 20 s), then dropped with a
  warning in the log. The controller keeps the setpoint; turning the emulation off does not
  restore the old one.
- **Hot water top-up:** a one-shot request in a single reply. The `hot_water_top_up` binary sensor
  shows when the top-up runs. It cannot be stopped from the bus, only by changing the operating
  mode on the display.
- **Fail-safe:** no reply while the switch is off, while the room temperature is unknown, stale
  (`room_sensor.timeout`) or outside 0–40 °C. If another room sensor answers on the bus, the
  component never transmits again until it reboots, and sets a warning.

When the replies stop, the display is expected to treat the room sensor as absent and heat on
the outdoor curve alone (how long it waits has not been measured).

## Scope / Limitations

- Developed against bus captures of an iTec Eco 5 and Eco 8 (display firmware 2.3.0); not
  verified on other models. The iTec XTR uses the same bus but partly different registers.
- Read-only for everything except the room sensor: settings, operating mode and hot water mode
  cannot be changed. That needs the Thermia Online session on slave 0x0F, which on the Eco is
  protected by a challenge–response that has not been reproduced.
- The settings page is only seen when the display sends it (see Entities).
- The emulated room sensor reply is sent from the main loop; the display waits about 145 ms for
  it, so a loop blocked for longer than that drops one reply (the next poll follows 4 s later).
- `operating_mode` shows the mode request, not whether the compressor runs; use `compressor`.

## Running tests

```bash
pytest tests/thermia_itec_eco/test_schema.py -v                       # schema tests, no hardware
esphome compile tests/components/thermia_itec_eco/test.esp32-idf.yaml

# Integration tests: the host build listens to a fake bus on a pseudo-terminal (including a
# 30 s capture of a real iTec Eco 5) and is driven over the native API on port 6063
esphome compile tests/components/thermia_itec_eco/test.host.yaml
pytest tests/thermia_itec_eco/test_thermia_itec_eco.py \
    --thermia-itec-eco-build-dir=tests/components/thermia_itec_eco/.esphome/build -v
```

The capture excerpt in `tests/thermia_itec_eco/itec_eco5_capture.log` is from
thermia-bus-sniffer (MIT), shared by the heat pump's owner.
