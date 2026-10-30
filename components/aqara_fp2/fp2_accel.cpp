#include "fp2_accel.h"

#ifdef USE_AQARA_FP2_ACCEL

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <cinttypes>
#include <cmath>
#include <cstdlib>

namespace esphome::aqara_fp2 {

static const char *const TAG = "aqara_fp2.accel";

// Register map of the on-board accelerometer.
static constexpr uint8_t REG_DATA_START = 0x02;  // X/Y/Z, 2 bytes each, 12-bit left-justified
static constexpr uint8_t REG_CONFIG_A = 0x11;
static constexpr uint8_t REG_CONFIG_B = 0x0F;
static constexpr uint8_t VAL_CONFIG_A = 0x0E;
static constexpr uint8_t VAL_CONFIG_B = 0x40;

// The stock firmware's debounce counts assume one decision per second: 10 samples, 100 ms apart.
static constexpr uint32_t SAMPLE_INTERVAL_MS = 100;

// Consecutive failed reads (one decision window) before the warning is set. A single read that
// times out now and then is skipped silently: the next one recovers the bus.
static constexpr uint8_t READ_FAILURES_BEFORE_WARNING = ACC_SAMPLE_COUNT;

// Radians to degrees.
static constexpr float SCALE_FACTOR = 57.2957795f;

// Indexed by Orientation.
PROGMEM_STRING_TABLE(OrientationStrings, "up", "up_tilt", "up_tilt_reverse", "side", "side_reverse", "down",
                     "down_tilt", "down_tilt_reverse", "invalid");

static const LogString *orientation_to_log_str(Orientation orientation) {
  return OrientationStrings::get_log_str(static_cast<uint8_t>(orientation), OrientationStrings::LAST_INDEX);
}

static constexpr bool in_range(int16_t value, int16_t low, int16_t high) { return low <= value && value <= high; }

void FP2Accel::setup() {
  if (!this->write_byte(REG_CONFIG_A, VAL_CONFIG_A) || !this->write_byte(REG_CONFIG_B, VAL_CONFIG_B)) {
    this->mark_failed(LOG_STR("Failed to configure accelerometer"));
  }
}

void FP2Accel::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Aqara FP2 accelerometer:\n"
                "  Sample interval: %" PRIu32 " ms",
                SAMPLE_INTERVAL_MS);
  LOG_I2C_DEVICE(this);
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Orientation", this->orientation_text_sensor_);
#endif
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Vibration", this->vibration_binary_sensor_);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Tilt angle", this->tilt_angle_sensor_);
#endif
}

void FP2Accel::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_sample_ms_ < SAMPLE_INTERVAL_MS) {
    return;
  }
  this->last_sample_ms_ = now;
  this->sample_();
}

bool FP2Accel::read_accel_xyz_(int16_t *x, int16_t *y, int16_t *z) {
  uint8_t data[6];
  if (!this->read_bytes(REG_DATA_START, data, sizeof(data))) {
    if (this->read_failures_ < READ_FAILURES_BEFORE_WARNING) {
      this->read_failures_++;
      ESP_LOGV(TAG, "Read failed (%u in a row)", this->read_failures_);
    }
    if (this->read_failures_ == READ_FAILURES_BEFORE_WARNING) {
      this->status_set_warning(LOG_STR("Failed to read accelerometer"));
    }
    return false;
  }
  this->read_failures_ = 0;
  this->status_clear_warning();

  // Each axis is a 12-bit two's-complement value, left-justified across two bytes.
  int16_t *out[3] = {x, y, z};
  for (uint8_t axis = 0; axis < 3; axis++) {
    uint16_t raw = static_cast<uint16_t>(data[axis * 2] >> 4) | static_cast<uint16_t>(data[axis * 2 + 1] << 4);
    if ((raw & 0x800) != 0) {
      raw |= 0xF000;  // sign-extend
    }
    *out[axis] = static_cast<int16_t>(raw);
  }
  return true;
}

void FP2Accel::sample_() {
  if (!this->read_accel_xyz_(&this->acc_x_buf_[this->samples_read_], &this->acc_y_buf_[this->samples_read_],
                             &this->acc_z_buf_[this->samples_read_])) {
    return;
  }
  this->samples_read_++;
  if (this->samples_read_ < ACC_SAMPLE_COUNT) {
    return;
  }
  this->samples_read_ = 0;

  // Average the buffered samples.
  int32_t sum_x = 0;
  int32_t sum_y = 0;
  int32_t sum_z = 0;
  for (uint8_t i = 0; i < ACC_SAMPLE_COUNT; i++) {
    sum_x += this->acc_x_buf_[i];
    sum_y += this->acc_y_buf_[i];
    sum_z += this->acc_z_buf_[i];
  }
  int16_t avg_x = static_cast<int16_t>(sum_x / ACC_SAMPLE_COUNT);
  int16_t avg_y = static_cast<int16_t>(sum_y / ACC_SAMPLE_COUNT);
  int16_t avg_z = static_cast<int16_t>(sum_z / ACC_SAMPLE_COUNT);

  // Energy: sum of squared deviations from the mean, used for vibration detection.
  int32_t var_x = 0;
  int32_t var_y = 0;
  int32_t var_z = 0;
  for (uint8_t i = 0; i < ACC_SAMPLE_COUNT; i++) {
    int32_t d = this->acc_x_buf_[i] - avg_x;
    var_x += d * d;
    d = this->acc_y_buf_[i] - avg_y;
    var_y += d * d;
    d = this->acc_z_buf_[i] - avg_z;
    var_z += d * d;
  }
  int32_t energy_sum = (var_x / ACC_SAMPLE_COUNT) + (var_y / ACC_SAMPLE_COUNT) + (var_z / ACC_SAMPLE_COUNT);

  this->process_samples_(avg_x, avg_y, avg_z, energy_sum);
}

void FP2Accel::process_samples_(int32_t acc_x, int32_t acc_y, int32_t acc_z, int32_t energy_sum) {
  // Inclination angles of each axis relative to the horizontal plane (-90..+90 deg).
  float dx = static_cast<float>(acc_x);
  float dy = static_cast<float>(acc_y);
  float dz = static_cast<float>(acc_z);
  int16_t angle_x = static_cast<int16_t>(std::atan2(dy, std::hypot(dx, dz)) * SCALE_FACTOR);
  int16_t angle_y = static_cast<int16_t>(std::atan2(dx, std::hypot(dy, dz)) * SCALE_FACTOR);
  int16_t angle_z = static_cast<int16_t>(std::atan2(dz, std::hypot(dx, dy)) * SCALE_FACTOR);

  // Output angle: 0 when vertical (Z pointing up/down), 90 when horizontal.
  this->output_angle_z_ = 90 - std::abs(angle_z);
  this->has_reading_ = true;

  // Orientation classification, with the stock firmware's angle windows (bounds inclusive).
  Orientation current = Orientation::ORIENTATION_INVALID;
  const bool y_flat = in_range(angle_y, -19, 19);
  const bool x_flat = in_range(angle_x, -19, 19);

  if (angle_z < -69) {
    if (y_flat && x_flat) {
      current = Orientation::ORIENTATION_UP;
    }
  } else if (angle_z <= -21) {
    if (y_flat && std::abs(angle_x) > 20) {
      current = angle_x < 0 ? Orientation::ORIENTATION_UP_TILT : Orientation::ORIENTATION_UP_TILT_REV;
    }
  } else if (angle_z > 70) {
    if (y_flat && x_flat) {
      current = Orientation::ORIENTATION_DOWN;
    }
  } else if (angle_z <= 19) {
    if (y_flat && std::abs(angle_x) > 50) {
      current = angle_x < 0 ? Orientation::ORIENTATION_SIDE : Orientation::ORIENTATION_SIDE_REV;
    }
  } else if (y_flat && in_range(angle_x, -69, 69)) {  // 20 <= angle_z <= 70
    current = angle_x < 0 ? Orientation::ORIENTATION_DOWN_TILT : Orientation::ORIENTATION_DOWN_TILT_REV;
  }

  // Debounce spurious INVALID readings by holding the last stable orientation.
  if (this->debounce_invalid_ < 30) {
    if (current == Orientation::ORIENTATION_INVALID) {
      this->debounce_invalid_++;
      current = this->stable_orientation_;
    } else {
      this->debounce_invalid_ = 0;
    }
  }

  Orientation stable = this->stable_orientation_;
  if (current != Orientation::ORIENTATION_INVALID) {
    if (current == Orientation::ORIENTATION_SIDE && this->stable_orientation_ == Orientation::ORIENTATION_DOWN_TILT &&
        this->debounce_side_ < 10) {
      this->debounce_side_++;
      current = this->stable_orientation_;
    } else {
      this->debounce_side_ = 0;
    }
    stable = current;

    if (current == Orientation::ORIENTATION_SIDE_REV &&
        this->stable_orientation_ == Orientation::ORIENTATION_DOWN_TILT_REV && this->debounce_side_rev_ < 10) {
      this->debounce_side_rev_++;
      current = this->stable_orientation_;
      stable = this->stable_orientation_;
    } else {
      this->debounce_side_rev_ = 0;
    }
  }

  Orientation previous_raw = this->raw_orientation_;
  this->stable_orientation_ = stable;
  this->raw_orientation_ = current;

  if (current != previous_raw) {
    ESP_LOGD(TAG, "Orientation %s -> %s (ax=%d ay=%d az=%d)", LOG_STR_ARG(orientation_to_log_str(previous_raw)),
             LOG_STR_ARG(orientation_to_log_str(current)), angle_x, angle_y, angle_z);
#ifdef USE_TEXT_SENSOR
    if (this->orientation_text_sensor_ != nullptr) {
      this->orientation_text_sensor_->publish_state(OrientationStrings::get_progmem_str(
          static_cast<uint8_t>(this->stable_orientation_), OrientationStrings::LAST_INDEX));
    }
#endif
  }

  // Vibration detection based on the change in energy between windows.
  int32_t delta = std::abs(energy_sum - this->last_energy_sum_);
  if (delta < 1000) {
    this->vibration_high_count_ = 0;
    this->vibration_low_count_++;
  } else {
    this->vibration_high_count_++;
    this->vibration_low_count_ = 0;
  }
  if (delta > 5000 || this->vibration_high_count_ > 4) {
    this->vibrating_ = true;
    this->vibration_high_count_ = 0;
  }
  if (this->vibration_low_count_ > 9) {
    this->vibrating_ = false;
    this->vibration_low_count_ = 0;
  }
  this->last_energy_sum_ = energy_sum;

  if (this->vibration_dedup_.next(this->vibrating_)) {
    ESP_LOGD(TAG, "Vibration %s", this->vibrating_ ? LOG_STR_LITERAL("ON") : LOG_STR_LITERAL("OFF"));
#ifdef USE_BINARY_SENSOR
    if (this->vibration_binary_sensor_ != nullptr) {
      this->vibration_binary_sensor_->publish_state(this->vibrating_);
    }
#endif
  }

#ifdef USE_SENSOR
  if (this->tilt_angle_sensor_ != nullptr && this->tilt_angle_dedup_.next(this->output_angle_z_)) {
    this->tilt_angle_sensor_->publish_state(this->output_angle_z_);
  }
#endif
}

}  // namespace esphome::aqara_fp2

#endif  // USE_AQARA_FP2_ACCEL
