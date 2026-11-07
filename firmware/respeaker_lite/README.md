# XMOS firmware for the ReSpeaker Lite

`respeaker_lite_i2s_dfu_firmware_48k_v1.1.0.bin` is the I2S firmware for the XMOS XU316 of the Seeed
ReSpeaker Lite: 48 kHz output, channel 0 processed for speech recognition, channel 1 for wake word
detection. The `respeaker_lite` component flashes it over I2C (see [docs/respeaker_lite.md](../../docs/respeaker_lite.md)).

| | |
|---|---|
| Version | 1.1.0 |
| Size | 274432 bytes |
| MD5 | `9297155d1bf3eb21a9d4db52a89ea0c6` |
| Source | Seeed, [respeaker/ReSpeaker_Lite](https://github.com/respeaker/ReSpeaker_Lite/tree/master/xmos_firmwares) `respeaker_lite_i2s_dfu_firmware_48k_v1.1.0_ch0-asr_ch1-mww.bin` (byte-identical; formatBCE ships the same file under this name) |

This binary is Seeed's work and is not covered by this repository's license (see [LICENSE](../../LICENSE)).

To use it from a config:

```yaml
respeaker_lite:
  firmware:
    url: https://github.com/remcom/esphome-component/raw/main/firmware/respeaker_lite/respeaker_lite_i2s_dfu_firmware_48k_v1.1.0.bin
    version: "1.1.0"
    md5: 9297155d1bf3eb21a9d4db52a89ea0c6
```

A new version needs a new file name: the `version` and `md5` in configs must change with it, and a
config pinned to an older ref keeps getting the file it was written for.
