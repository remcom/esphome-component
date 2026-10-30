#pragma once

#include "esphome/core/defines.h"

#ifdef USE_AQARA_FP2_ACCEL

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/i2c/i2c.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <array>
#include <cstdint>

namespace esphome::aqara_fp2 {

// Number of raw samples averaged before an orientation/vibration decision is made.
static constexpr uint8_t ACC_SAMPLE_COUNT = 10;

// Mounting orientation reported to the radar. The values match the codes the stock
// Aqara firmware sends back to the radar coprocessor in a "device direction" query.
enum class Orientation : uint8_t {
  ORIENTATION_UP = 0,
  ORIENTATION_UP_TILT = 1,
  ORIENTATION_UP_TILT_REV = 2,
  ORIENTATION_SIDE = 3,
  ORIENTATION_SIDE_REV = 4,
  ORIENTATION_DOWN = 5,
  ORIENTATION_DOWN_TILT = 6,
  ORIENTATION_DOWN_TILT_REV = 7,
  ORIENTATION_INVALID = 8,
};

// On-board accelerometer of the FP2. The radar queries its mounting orientation and tilt.
class FP2Accel : public Component, public i2c::I2CDevice {
#ifdef USE_TEXT_SENSOR
  // Human-readable mounting orientation.
  SUB_TEXT_SENSOR(orientation)
#endif
#ifdef USE_BINARY_SENSOR
  // True while the device is being moved/tapped.
  SUB_BINARY_SENSOR(vibration)
#endif
#ifdef USE_SENSOR
  // Tilt angle of the Z axis relative to vertical, in degrees.
  SUB_SENSOR(tilt_angle)
#endif

 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Accessors consumed by FP2Component when the radar asks for its mounting orientation.
  Orientation get_orientation() const { return this->stable_orientation_; }
  uint8_t get_output_angle_z() const { return static_cast<uint8_t>(this->output_angle_z_); }

 protected:
  bool read_accel_xyz_(int16_t *x, int16_t *y, int16_t *z);
  void sample_();
  void process_samples_(int32_t acc_x, int32_t acc_y, int32_t acc_z, int32_t energy_sum);

  uint32_t last_sample_ms_{0};

  // Rolling sample buffers.
  std::array<int16_t, ACC_SAMPLE_COUNT> acc_x_buf_{};
  std::array<int16_t, ACC_SAMPLE_COUNT> acc_y_buf_{};
  std::array<int16_t, ACC_SAMPLE_COUNT> acc_z_buf_{};
  uint8_t samples_read_{0};

  // Orientation state machine.
  Orientation stable_orientation_{Orientation::ORIENTATION_INVALID};
  Orientation raw_orientation_{Orientation::ORIENTATION_INVALID};
  int16_t output_angle_z_{0};
  uint8_t debounce_invalid_{0};
  uint8_t debounce_side_{0};
  uint8_t debounce_side_rev_{0};
#ifdef USE_SENSOR
  Deduplicator<int16_t> tilt_angle_dedup_;
#endif

  // Vibration detection.
  int32_t last_energy_sum_{0};
  uint8_t vibration_high_count_{0};
  uint8_t vibration_low_count_{0};
  bool vibrating_{false};
  // Publishes the first state too, so a device at rest reports "off" instead of unknown.
  Deduplicator<bool> vibration_dedup_;
};

}  // namespace esphome::aqara_fp2

#endif  // USE_AQARA_FP2_ACCEL
