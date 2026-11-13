# respeaker_lite — XMOS audio processor of the Seeed ReSpeaker Lite

Controls the XMOS XU316 audio processor of the Seeed ReSpeaker Lite Voice Kit
(XIAO ESP32-S3 version) over I2C:

- resets the XMOS at boot and reads its firmware version
- flashes a bundled XMOS firmware over I2C (DFU) when the XMOS runs another version
- reports the hardware microphone mute state (the mute button on the board)
- mutes and unmutes the speaker amplifier

The audio itself goes over I2S and uses ESPHome's stock components (`i2s_audio`, `resampler`,
`mixer`, `aic3204`). For a complete voice satellite, see [the example configuration](#example-voice-satellite).

Ported from [formatBCE/Respeaker-Lite-ESPHome-integration](https://github.com/formatBCE/Respeaker-Lite-ESPHome-integration).

## Example: voice satellite

[`examples/respeaker_lite/`](../examples/respeaker_lite) has a ready-to-use Home Assistant voice satellite:

- wake words `okay_nabu`, `hey_jarvis` and `hey_mycroft` with a sensitivity select, and "stop" to end a
  ringing timer or a long answer
- voice assistant with timers (the timer rings until stopped, at most 15 minutes)
- media player for Home Assistant and [Sendspin](https://www.sendspin-audio.com) (Music Assistant
  multi-room, with pairing code; 24-bit FLAC/PCM, set `media_bits_per_sample: "16"` to save RAM),
  announcements ducking the music, and an *Announce volume* number for announcements on an idle player
- wake, mute and timer sounds of the Home Assistant Voice PE, each sound switchable, and a spoken error
  when the Home Assistant Cloud subscription has expired
- LED status (waiting for speech, listening, thinking, replying, error), user button (stops a timer, the
  assistant, an announcement or music, pausing the whole Sendspin group when grouped; otherwise talks)
  and a mic mute switch

1. Copy [`respeaker-lite.yaml`](../examples/respeaker_lite/respeaker-lite.yaml) into your ESPHome
   config directory (rename it to your device name if you like).
2. Add `wifi_ssid`, `wifi_password` and `api_encryption_key` to your `secrets.yaml`
   (see [`secrets.yaml.example`](../examples/respeaker_lite/secrets.yaml.example)).
3. Install it over USB the first time. The first boot flashes the XMOS firmware, which takes about a
   minute; the LED pulses white meanwhile and blinks green when done.

The device config loads everything else as a remote package:

```yaml
packages:
  respeaker_lite: github://remcom/esphome-component/examples/respeaker_lite/respeaker-lite-base.yaml@main
```

Override any substitution of [`respeaker-lite-base.yaml`](../examples/respeaker_lite/respeaker-lite-base.yaml)
(`name`, `friendly_name`, the XMOS firmware) in your own `substitutions:`, and extend entities with
`id: !extend <id>`. To change more, copy the base file instead of loading it remotely.

The XMOS drives the I2S clocks at 48 kHz for microphones and speaker together, so the example uses one
full-duplex `i2s_audio` bus and ESPHome's `resampler` microphone (48 kHz → 16 kHz) for wake word and
speech: channel 1 (processed for wake words) feeds `micro_wake_word`, channel 0 (processed for speech
recognition) the voice assistant. `respeaker-lite.yaml` shows how to send both channels to Home Assistant.
Do not set `prefer_internal_ram` on `micro_wake_word`: it leaves too little internal heap for the I2S
speaker, which then crackles, and the wake word stops responding.

Sendspin streams arrive at 24 bits (`media_bits_per_sample`); the media resampler passes the bit depth
through and the mixer converts to the 32-bit I2S output. Announcements stay 16-bit. Opus is not offered to
Sendspin, as it decodes to 16 bits only.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/remcom/esphome-component
      ref: main
    components:
      - respeaker_lite
```

Requires ESPHome 2026.11.0 or newer and an ESP32 (the ReSpeaker Lite has a XIAO ESP32-S3).

## Configuration

```yaml
i2c:
  - id: internal_i2c
    sda: GPIO5
    scl: GPIO6
    frequency: 400kHz

respeaker_lite:
  id: respeaker
  i2c_id: internal_i2c
  reset_pin: GPIO2
  firmware:
    url: https://github.com/remcom/esphome-component/raw/main/firmware/respeaker_lite/respeaker_lite_i2s_dfu_firmware_48k_v1.1.0.bin
    version: "1.1.0"
    md5: 9297155d1bf3eb21a9d4db52a89ea0c6
    on_begin:
      - logger.log: XMOS update started
    on_progress:
      - logger.log:
          format: "XMOS update %.0f%%"
          args: [x]
    on_end:
      - logger.log: XMOS update done
    on_error:
      - logger.log:
          format: "XMOS update failed (%u)"
          args: [x]

binary_sensor:
  - platform: respeaker_lite
    mic_mute:
      name: Mic muted

text_sensor:
  - platform: respeaker_lite
    firmware_version:
      name: XMOS firmware version
```

### `respeaker_lite`

| Key | Default | Description |
|-----|---------|-------------|
| `reset_pin` | required | Reset pin of the XMOS (`GPIO2` on the ReSpeaker Lite). |
| `address` | `0x42` | I2C address of the XMOS. |
| `i2c_id` | | I2C bus, when there are several. |
| `firmware` | | XMOS firmware to bundle and flash, see below. Without it the component only reads the version and mute state. |

### `firmware`

The firmware is downloaded (or read) at compile time, checked against `md5`, and embedded in the
ESP32 image (about 270 KB for v1.1.0). This repository has a copy of Seeed's v1.1.0 48 kHz I2S firmware in
[`firmware/respeaker_lite/`](../firmware/respeaker_lite), with its source and checksum.

| Key | Default | Description |
|-----|---------|-------------|
| `url` | | Download URL of the firmware `.bin`. Exactly one of `url` and `path` is required. |
| `path` | | Local firmware file, relative to the config. |
| `version` | required | Version of that firmware (`major.minor.patch`). The XMOS is flashed when it reports another version, so a wrong value reflashes on every boot. |
| `md5` | required | MD5 checksum of the file; validation fails when it does not match. |
| `auto_update_min_version` | `1.0.8` | Oldest XMOS version that is updated automatically. The I2C DFU of older firmware can erase its own factory image, so below this version the component does not flash and marks itself failed: flash the firmware over USB first. |
| `on_begin` | | Automation run when an update starts. |
| `on_progress` | | Automation run about every second during an update; `x` (float) is the progress in percent. |
| `on_end` | | Automation run after the XMOS reports the new version. |
| `on_error` | | Automation run when an update fails; `x` (uint8) is 1 (I2C error), 3 (timeout) or 4 (XMOS reported an error or the wrong version). |

### Entities

| Platform | Key | Description |
|----------|-----|-------------|
| `binary_sensor` | `mic_mute` | Hardware microphone mute state, read every 50 ms. On = muted. |
| `text_sensor` | `firmware_version` | XMOS firmware version, e.g. `1.1.0`. |

### Actions

| Action | Description |
|--------|-------------|
| `respeaker_lite.mute_speaker` | Mutes the speaker amplifier, e.g. in `on_shutdown` to prevent a pop on restart. |
| `respeaker_lite.unmute_speaker` | Unmutes the speaker amplifier. |
| `respeaker_lite.flash` | Flashes the bundled firmware now, also when the version already matches. Ignored (with a warning) during the first 3 s after boot, while an update runs, and when the component has failed. |

From a lambda: `id(respeaker).is_failed()`, `id(respeaker).is_dfu_running()` and
`id(respeaker).get_firmware_version()` (`major << 16 | minor << 8 | patch`, 0 until read).

### Mic mute

The XMOS owns the microphone mute; the ESP32 can only toggle it by pulsing `GPIO4` (the same as pressing
the mute button) and read the state back. The example builds a `Mic mute` switch from a GPIO switch and
the `mic_mute` binary sensor.

## How it works

- `setup()` pulses the reset pin and reads the version 3 s later. When a firmware is bundled and the
  version differs, it starts the DFU transfer in `loop()`: 128-byte blocks, as many per loop as the
  XMOS accepts within 20 ms (it reports how long to wait before the next status request). Then it
  reboots the XMOS and checks the new version.
- Without a `mic_mute` sensor, `loop()` is switched off once the boot check is done; it only runs again
  for a firmware update.
- Until the version is read (and, with a bundled firmware, matches), `can_proceed()` holds the setup of
  the components that come after it, so audio components start against the expected firmware. The
  setup priority is just below lights, so the LED can show the update.
- An XMOS that does not answer, firmware older than `auto_update_min_version`, or a failed update marks
  the component failed (the rest of the setup continues). The example LED then pulses red.

## Limitations

- The I2C protocol (DFU, mute, version) is the upstream one, but the code is restructured and has so far
  only been compile-tested, not run on a device.
- The mic mute state is polled over I2C (20 requests per second) while a `mic_mute` sensor is configured.
- A failed update leaves the XMOS without audio until it is flashed again (reboot to retry, or flash it
  over USB).
