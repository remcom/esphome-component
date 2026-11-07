#include "respeaker_lite.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::respeaker_lite {

static const char *const TAG = "respeaker_lite";

static constexpr uint8_t version_major(uint32_t version) { return (version >> 16) & 0xFF; }
static constexpr uint8_t version_minor(uint32_t version) { return (version >> 8) & 0xFF; }
static constexpr uint8_t version_patch(uint32_t version) { return version & 0xFF; }

/// Time from `since` to `now`; 0 when `since` was taken later than `now` (millis() in an
/// action vs. the loop start time).
static uint32_t elapsed_ms(uint32_t now, uint32_t since) {
  const auto diff = static_cast<int32_t>(now - since);
  return diff > 0 ? static_cast<uint32_t>(diff) : 0;
}

void RespeakerLite::setup() {
  // Reset the XMOS (active high) and give it time to boot
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  delay(1);
  this->reset_pin_->digital_write(false);
  this->set_timeout(BOOT_DELAY_MS, [this]() { this->check_version_after_boot_(); });
}

void RespeakerLite::check_version_after_boot_() {
  this->boot_checked_ = true;
  if (!this->dfu_get_version_()) {
    this->mark_failed(LOG_STR("No response from the XMOS"));
    return;
  }
  if (!this->has_firmware_() || this->version_matches_()) {
    this->set_ready_();
    return;
  }
  const uint32_t found = this->firmware_version_;
  const uint32_t bundled = this->firmware_bin_version_;
  if (found < this->auto_update_min_version_) {
    ESP_LOGW(TAG,
             "XMOS firmware %u.%u.%u is too old to update over I2C (its DFU can erase the factory image); flash "
             "%u.%u.%u over USB",
             version_major(found), version_minor(found), version_patch(found), version_major(bundled),
             version_minor(bundled), version_patch(bundled));
    this->mark_failed(LOG_STR("XMOS firmware too old to update safely"));
    return;
  }
  ESP_LOGW(TAG, "XMOS firmware is %u.%u.%u, expected %u.%u.%u: updating", version_major(found), version_minor(found),
           version_patch(found), version_major(bundled), version_minor(bundled), version_patch(bundled));
  this->start_dfu_update();
}

bool RespeakerLite::can_proceed() {
  // Hold the other components until the version is read and, with a bundled firmware, matches
  return this->is_failed() || (this->ready_ && !this->is_dfu_running());
}

void RespeakerLite::dump_config() {
  ESP_LOGCONFIG(TAG, "ReSpeaker Lite:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  if (this->firmware_version_ != 0) {
    ESP_LOGCONFIG(TAG, "  XMOS firmware: %u.%u.%u", version_major(this->firmware_version_),
                  version_minor(this->firmware_version_), version_patch(this->firmware_version_));
  }
  if (this->has_firmware_()) {
    ESP_LOGCONFIG(TAG,
                  "  Bundled firmware: %u.%u.%u (%" PRIu32 " bytes)\n"
                  "  Auto-update from: %u.%u.%u",
                  version_major(this->firmware_bin_version_), version_minor(this->firmware_bin_version_),
                  version_patch(this->firmware_bin_version_), this->firmware_bin_length_,
                  version_major(this->auto_update_min_version_), version_minor(this->auto_update_min_version_),
                  version_patch(this->auto_update_min_version_));
  }
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Mic Mute", this->mic_mute_binary_sensor_);
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Firmware Version", this->firmware_version_text_sensor_);
#endif
}

void RespeakerLite::set_ready_() {
  this->ready_ = true;
#ifdef USE_BINARY_SENSOR
  if (this->mic_mute_binary_sensor_ != nullptr)
    return;
#endif
  // Nothing to poll: loop() only runs again for a firmware update
  this->disable_loop();
}

void RespeakerLite::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->is_dfu_running()) {
    this->dfu_step_(now);
    return;
  }
#ifdef USE_BINARY_SENSOR
  if (this->ready_ && this->mic_mute_binary_sensor_ != nullptr &&
      now - this->mic_mute_last_poll_ms_ >= MIC_MUTE_POLL_MS) {
    this->mic_mute_last_poll_ms_ = now;
    this->poll_mic_mute_();
  }
#endif
}

void RespeakerLite::start_dfu_update() {
  if (!this->has_firmware_()) {
    ESP_LOGE(TAG, "No firmware bundled: add a firmware block to flash the XMOS");
    return;
  }
  if (this->is_dfu_running()) {
    ESP_LOGW(TAG, "Firmware update already running");
    return;
  }
  // A failed component has no loop() to run the update, and before the boot check the XMOS may still be booting
  if (this->is_failed() || !this->boot_checked_) {
    ESP_LOGW(TAG, "XMOS not ready, firmware update not started");
    return;
  }
  ESP_LOGI(TAG, "Updating XMOS firmware from %u.%u.%u to %u.%u.%u", version_major(this->firmware_version_),
           version_minor(this->firmware_version_), version_patch(this->firmware_version_),
           version_major(this->firmware_bin_version_), version_minor(this->firmware_bin_version_),
           version_patch(this->firmware_bin_version_));
  this->dfu_begin_callback_.call();

  if (!this->dfu_set_alternate_()) {
    this->dfu_fail_(DFU_ERROR_COMMUNICATION);
    return;
  }
  const uint32_t now = millis();
  this->dfu_phase_ = DfuPhase::DOWNLOAD;
  this->dfu_state_ = 0;
  this->dfu_bytes_written_ = 0;
  this->dfu_status_read_ms_ = now;
  this->dfu_status_delay_ms_ = 0;
  this->dfu_last_ready_ms_ = now;
  this->dfu_last_progress_ms_ = now;
  this->dfu_start_ms_ = now;
  this->enable_loop();
}

void RespeakerLite::dfu_step_(uint32_t now) {
  if (elapsed_ms(now, this->dfu_last_ready_ms_) > DFU_TIMEOUT_MS) {
    ESP_LOGE(TAG, "Firmware update timed out at %" PRIu32 " of %" PRIu32 " bytes (DFU state %u, status %u)",
             this->dfu_bytes_written_, this->firmware_bin_length_, this->dfu_state_, this->dfu_status_);
    this->dfu_fail_(DFU_ERROR_TIMEOUT);
    return;
  }
  if (this->dfu_state_ == DFU_INT_STATE_DFU_ERROR) {
    ESP_LOGE(TAG, "Firmware update failed at %" PRIu32 " of %" PRIu32 " bytes (DFU status %u)",
             this->dfu_bytes_written_, this->firmware_bin_length_, this->dfu_status_);
    this->dfu_fail_(DFU_ERROR_FAILED);
    return;
  }

  switch (this->dfu_phase_) {
    case DfuPhase::DOWNLOAD: {
      // Send blocks for up to DFU_LOOP_BUDGET_MS per loop, as long as the XMOS is ready right away;
      // one block per loop would take a loop interval per 128 bytes
      const uint32_t start = millis();
      uint32_t time = now;
      while (this->dfu_check_ready_(time)) {
        const uint32_t remaining = this->firmware_bin_length_ - this->dfu_bytes_written_;
        const auto length = static_cast<uint8_t>(std::min<uint32_t>(remaining, DFU_MAX_XFER));
        if (!this->dfu_send_block_(length)) {
          this->dfu_fail_(DFU_ERROR_COMMUNICATION);
          return;
        }
        this->dfu_bytes_written_ += length;
        const bool done = this->dfu_bytes_written_ >= this->firmware_bin_length_;
        this->dfu_publish_progress_(time, done);
        if (done) {
          this->dfu_phase_ = DfuPhase::FINISH;
          return;
        }
        time = millis();
        if (time - start >= DFU_LOOP_BUDGET_MS)
          return;
      }
      return;
    }

    case DfuPhase::FINISH:
      if (!this->dfu_check_ready_(now))
        return;
      // An empty download request ends the download
      if (!this->dfu_send_block_(0)) {
        this->dfu_fail_(DFU_ERROR_COMMUNICATION);
        return;
      }
      this->dfu_phase_ = DfuPhase::REBOOT;
      return;

    case DfuPhase::REBOOT:
      if (!this->dfu_check_ready_(now))
        return;
      ESP_LOGI(TAG, "Firmware written in %" PRIu32 " s, rebooting the XMOS",
               elapsed_ms(now, this->dfu_start_ms_) / 1000);
      if (!this->dfu_reboot_()) {
        this->dfu_fail_(DFU_ERROR_COMMUNICATION);
        return;
      }
      this->dfu_last_progress_ms_ = now;
      this->dfu_phase_ = DfuPhase::VERIFY;
      return;

    case DfuPhase::VERIFY:
      // The XMOS does not answer while it reboots; retry until DFU_TIMEOUT_MS after the reboot command
      if (elapsed_ms(now, this->dfu_last_progress_ms_) < DFU_VERIFY_INTERVAL_MS)
        return;
      this->dfu_last_progress_ms_ = now;
      if (!this->dfu_get_version_())
        return;
      if (!this->version_matches_()) {
        ESP_LOGE(TAG, "XMOS reports %u.%u.%u after the update", version_major(this->firmware_version_),
                 version_minor(this->firmware_version_), version_patch(this->firmware_version_));
        this->dfu_fail_(DFU_ERROR_FAILED);
        return;
      }
      ESP_LOGI(TAG, "Firmware update complete");
      this->dfu_phase_ = DfuPhase::IDLE;
      this->set_ready_();
      this->dfu_end_callback_.call();
      return;

    case DfuPhase::IDLE:
      return;
  }
}

void RespeakerLite::dfu_fail_(DfuError error) {
  this->dfu_phase_ = DfuPhase::IDLE;
  this->dfu_error_callback_.call(error);
  // The XMOS has no valid firmware or an unknown one now: no audio until it is flashed again
  this->mark_failed(LOG_STR("XMOS firmware update failed"));
}

void RespeakerLite::dfu_publish_progress_(uint32_t now, bool force) {
  if (!force && elapsed_ms(now, this->dfu_last_progress_ms_) < DFU_PROGRESS_INTERVAL_MS)
    return;
  this->dfu_last_progress_ms_ = now;
  const float percentage = this->dfu_bytes_written_ * 100.0f / this->firmware_bin_length_;
  ESP_LOGD(TAG, "Update progress: %.1f%%", percentage);
  this->dfu_progress_callback_.call(percentage);
}

bool RespeakerLite::dfu_check_ready_(uint32_t now) {
  // The XMOS tells how long to wait before the next status request
  if (elapsed_ms(now, this->dfu_status_read_ms_) < this->dfu_status_delay_ms_)
    return false;
  if (!this->dfu_get_status_(now))
    return false;
  if (this->dfu_state_ == DFU_INT_STATE_DFU_IDLE || this->dfu_state_ == DFU_INT_STATE_DFU_DNLOAD_IDLE ||
      this->dfu_state_ == DFU_INT_STATE_DFU_MANIFEST_WAIT_RESET) {
    this->dfu_last_ready_ms_ = now;
    return true;
  }
  return false;
}

bool RespeakerLite::dfu_get_status_(uint32_t now) {
  const uint8_t request[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_COMMAND_GETSTATUS | COMMAND_READ_BIT, 6};
  // status, DFU status, poll timeout (24 bit, little endian, ms), DFU state
  uint8_t response[6];
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK ||
      this->read(response, sizeof(response)) != i2c::ERROR_OK || response[0] != CONTROL_STATUS_DONE) {
    ESP_LOGW(TAG, "Reading the DFU status failed");
    return false;
  }
  this->dfu_status_read_ms_ = now;
  this->dfu_status_ = response[1];
  this->dfu_status_delay_ms_ = encode_uint24(response[4], response[3], response[2]);
  this->dfu_state_ = response[5];
  ESP_LOGVV(TAG, "DFU state %u, status %u, next poll in %" PRIu32 " ms", this->dfu_state_, this->dfu_status_,
            this->dfu_status_delay_ms_);
  return true;
}

bool RespeakerLite::dfu_get_version_() {
  const uint8_t request[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_COMMAND_GETVERSION | COMMAND_READ_BIT, 4};
  // status, major, minor, patch
  uint8_t response[4];
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK ||
      this->read(response, sizeof(response)) != i2c::ERROR_OK || response[0] != CONTROL_STATUS_DONE) {
    ESP_LOGW(TAG, "Reading the XMOS firmware version failed");
    return false;
  }
  this->firmware_version_ = encode_uint24(response[1], response[2], response[3]);
  ESP_LOGI(TAG, "XMOS firmware version: %u.%u.%u", response[1], response[2], response[3]);
#ifdef USE_TEXT_SENSOR
  if (this->firmware_version_text_sensor_ != nullptr) {
    char version[12];  // "255.255.255"
    snprintf(version, sizeof(version), "%u.%u.%u", response[1], response[2], response[3]);
    this->firmware_version_text_sensor_->publish_state(version);
  }
#endif
  return true;
}

bool RespeakerLite::dfu_send_block_(uint8_t length) {
  // resid, command, payload length (block length field + data), block length (16 bit LE), data
  uint8_t request[5 + DFU_MAX_XFER] = {DFU_CONTROLLER_SERVICER_RESID, DFU_COMMAND_DNLOAD, DFU_MAX_XFER + 2, length, 0};
  if (length > 0) {
    std::memcpy(&request[5], this->firmware_bin_ + this->dfu_bytes_written_, length);
  }
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "DFU download request failed");
    return false;
  }
  return true;
}

bool RespeakerLite::dfu_reboot_() {
  const uint8_t request[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_COMMAND_REBOOT, 1, 0};
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "DFU reboot request failed");
    return false;
  }
  return true;
}

bool RespeakerLite::dfu_set_alternate_() {
  const uint8_t request[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_COMMAND_SETALTERNATE, 1, DFU_ALTERNATE_UPGRADE};
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "DFU set alternate request failed");
    return false;
  }
  return true;
}

void RespeakerLite::poll_mic_mute_() {
#ifdef USE_BINARY_SENSOR
  const uint8_t request[] = {CONFIGURATION_SERVICER_RESID, CONFIGURATION_CMD_MIC_MUTE | COMMAND_READ_BIT, 1};
  // status, mute state
  uint8_t response[2];
  const bool ok = this->write(request, sizeof(request)) == i2c::ERROR_OK &&
                  this->read(response, sizeof(response)) == i2c::ERROR_OK && response[0] == CONTROL_STATUS_DONE;
  if (this->status_comm_result(ok, LOG_STR("Reading the mic mute state failed")))
    this->mic_mute_binary_sensor_->publish_state(response[1] == 1);
#endif
}

void RespeakerLite::write_speaker_mute_(bool mute) {
  const uint8_t request[] = {CONFIGURATION_SERVICER_RESID, CONFIGURATION_CMD_SPEAKER_MUTE, 1,
                             static_cast<uint8_t>(mute ? 0 : 1)};
  if (this->write(request, sizeof(request)) != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "%s the speaker failed", mute ? "Muting" : "Unmuting");
  }
}

}  // namespace esphome::respeaker_lite
