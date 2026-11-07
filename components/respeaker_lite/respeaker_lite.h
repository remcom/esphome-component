#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome::respeaker_lite {

// Control protocol of the XMOS XU316 (sln_voice device control over I2C): a request is
// resource ID, command ID (| READ_BIT for reads), payload length, payload. A read answer
// starts with a status byte (ControlStatus).
static constexpr uint8_t DFU_CONTROLLER_SERVICER_RESID = 240;
static constexpr uint8_t CONFIGURATION_SERVICER_RESID = 241;
static constexpr uint8_t COMMAND_READ_BIT = 0x80;

// Configuration servicer commands of the ReSpeaker Lite firmware
static constexpr uint8_t CONFIGURATION_CMD_SPEAKER_MUTE = 0x10;  // write: 0 = muted, 1 = unmuted
static constexpr uint8_t CONFIGURATION_CMD_MIC_MUTE = 0x01;      // read: 1 = muted

/// Maximum firmware bytes per DFU download request
static constexpr uint8_t DFU_MAX_XFER = 128;
/// The XMOS must report a ready DFU state within this time, or the update fails
static constexpr uint32_t DFU_TIMEOUT_MS = 4000;
/// Longest time loop() spends sending firmware blocks
static constexpr uint32_t DFU_LOOP_BUDGET_MS = 20;
/// Time between progress callbacks
static constexpr uint32_t DFU_PROGRESS_INTERVAL_MS = 1000;
/// Time between version requests while the XMOS reboots after an update
static constexpr uint32_t DFU_VERIFY_INTERVAL_MS = 500;
/// Time the XMOS needs to boot after a reset
static constexpr uint32_t BOOT_DELAY_MS = 3000;
/// Polling interval of the hardware microphone mute state
static constexpr uint32_t MIC_MUTE_POLL_MS = 50;

enum ControlStatus : uint8_t {
  CONTROL_STATUS_DONE = 0,
  CONTROL_STATUS_WAIT = 1,
  CONTROL_STATUS_INVALID = 3,
};

// From https://github.com/xmos/sln_voice/blob/develop/examples/ffva/src/dfu_int/dfu_state_machine.h
enum DfuAlternate : uint8_t {
  DFU_ALTERNATE_FACTORY = 0,
  DFU_ALTERNATE_UPGRADE = 1,
};

enum DfuIntState : uint8_t {
  DFU_INT_STATE_APP_IDLE = 0,
  DFU_INT_STATE_APP_DETACH,
  DFU_INT_STATE_DFU_IDLE,
  DFU_INT_STATE_DFU_DNLOAD_SYNC,
  DFU_INT_STATE_DFU_DNBUSY,
  DFU_INT_STATE_DFU_DNLOAD_IDLE,
  DFU_INT_STATE_DFU_MANIFEST_SYNC,
  DFU_INT_STATE_DFU_MANIFEST,
  DFU_INT_STATE_DFU_MANIFEST_WAIT_RESET,
  DFU_INT_STATE_DFU_UPLOAD_IDLE,
  DFU_INT_STATE_DFU_ERROR,
};

enum DfuCommand : uint8_t {
  DFU_COMMAND_DETACH = 0,
  DFU_COMMAND_DNLOAD = 1,
  DFU_COMMAND_UPLOAD = 2,
  DFU_COMMAND_GETSTATUS = 3,
  DFU_COMMAND_CLRSTATUS = 4,
  DFU_COMMAND_GETSTATE = 5,
  DFU_COMMAND_ABORT = 6,
  DFU_COMMAND_SETALTERNATE = 64,
  DFU_COMMAND_TRANSFERBLOCK = 65,
  DFU_COMMAND_GETVERSION = 88,
  DFU_COMMAND_REBOOT = 89,
};

/// Error code passed to on_error
enum DfuError : uint8_t {
  DFU_ERROR_COMMUNICATION = 1,
  DFU_ERROR_TIMEOUT = 3,
  DFU_ERROR_FAILED = 4,
};

/// Progress of a firmware update of the XMOS
enum class DfuPhase : uint8_t {
  IDLE,
  DOWNLOAD,  ///< Sending firmware blocks
  FINISH,    ///< Sending the empty block that ends the download
  REBOOT,    ///< Waiting for the XMOS to accept the reboot command
  VERIFY,    ///< Waiting for the rebooted XMOS to report the new version
};

/**
 * @brief XMOS XU316 audio processor of the Seeed ReSpeaker Lite, over I2C.
 *
 * Resets the XMOS at boot, reads its firmware version and, when a firmware is
 * bundled and the version differs, flashes it over I2C DFU. can_proceed() holds
 * the rest of the setup until the version matches, so audio components start
 * against the expected firmware. Also reads the hardware microphone mute state
 * and mutes or unmutes the speaker amplifier.
 */
class RespeakerLite : public Component, public i2c::I2CDevice {
 public:
  explicit RespeakerLite(GPIOPin *reset_pin) : reset_pin_(reset_pin) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  bool can_proceed() override;
  // Below LightState (HARDWARE - 1): can_proceed() blocks setup, and DFU automations
  // drive lights meanwhile, so the lights must be set up first. Above TemplateSwitch
  // (HARDWARE - 2).
  float get_setup_priority() const override { return setup_priority::HARDWARE - 1.5f; }

  void set_firmware(const uint8_t *data, uint32_t length, uint8_t major, uint8_t minor, uint8_t patch) {
    this->firmware_bin_ = data;
    this->firmware_bin_length_ = length;
    this->firmware_bin_version_ = encode_uint24(major, minor, patch);
  }
  void set_auto_update_min_version(uint8_t major, uint8_t minor, uint8_t patch) {
    this->auto_update_min_version_ = encode_uint24(major, minor, patch);
  }
#ifdef USE_BINARY_SENSOR
  void set_mic_mute_binary_sensor(binary_sensor::BinarySensor *sensor) { this->mic_mute_binary_sensor_ = sensor; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_firmware_version_text_sensor(text_sensor::TextSensor *sensor) {
    this->firmware_version_text_sensor_ = sensor;
  }
#endif

  template<typename F> void add_on_dfu_begin_callback(F &&callback) {
    this->dfu_begin_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_dfu_progress_callback(F &&callback) {
    this->dfu_progress_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_dfu_end_callback(F &&callback) {
    this->dfu_end_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_dfu_error_callback(F &&callback) {
    this->dfu_error_callback_.add(std::forward<F>(callback));
  }

  /// Flash the bundled firmware, also when the XMOS already runs that version
  void start_dfu_update();
  bool is_dfu_running() const { return this->dfu_phase_ != DfuPhase::IDLE; }
  void mute_speaker() { this->write_speaker_mute_(true); }
  void unmute_speaker() { this->write_speaker_mute_(false); }
  /// Firmware version reported by the XMOS as major << 16 | minor << 8 | patch; 0 = not read
  uint32_t get_firmware_version() const { return this->firmware_version_; }

 protected:
  bool has_firmware_() const { return this->firmware_bin_ != nullptr && this->firmware_bin_length_ > 0; }
  bool version_matches_() const { return this->firmware_version_ == this->firmware_bin_version_; }
  void check_version_after_boot_();
  /// Boot check (and update) done: enables mic mute polling, or stops loop() when there is nothing to poll
  void set_ready_();

  void dfu_step_(uint32_t now);
  void dfu_fail_(DfuError error);
  void dfu_publish_progress_(uint32_t now, bool force);
  bool dfu_check_ready_(uint32_t now);
  bool dfu_get_status_(uint32_t now);
  bool dfu_get_version_();
  bool dfu_send_block_(uint8_t length);
  bool dfu_reboot_();
  bool dfu_set_alternate_();

  void poll_mic_mute_();
  void write_speaker_mute_(bool mute);

  GPIOPin *reset_pin_;
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *mic_mute_binary_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *firmware_version_text_sensor_{nullptr};
#endif

  LazyCallbackManager<void()> dfu_begin_callback_;
  LazyCallbackManager<void(float)> dfu_progress_callback_;
  LazyCallbackManager<void()> dfu_end_callback_;
  LazyCallbackManager<void(uint8_t)> dfu_error_callback_;

  const uint8_t *firmware_bin_{nullptr};
  uint32_t firmware_bin_length_{0};
  uint32_t firmware_bin_version_{0};
  uint32_t auto_update_min_version_{0};
  uint32_t firmware_version_{0};

  DfuPhase dfu_phase_{DfuPhase::IDLE};
  uint8_t dfu_state_{0};
  uint8_t dfu_status_{0};
  uint32_t dfu_bytes_written_{0};
  uint32_t dfu_status_read_ms_{0};
  uint32_t dfu_status_delay_ms_{0};
  uint32_t dfu_last_ready_ms_{0};
  uint32_t dfu_last_progress_ms_{0};
  uint32_t dfu_start_ms_{0};

  uint32_t mic_mute_last_poll_ms_{0};
  /// Set once the boot-time version check has run
  bool boot_checked_{false};
  /// Set once the boot-time version check is done (and the update, if one was needed)
  bool ready_{false};
};

}  // namespace esphome::respeaker_lite
