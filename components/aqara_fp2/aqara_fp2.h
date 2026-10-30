#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_EVENT
#include "esphome/components/event/event.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

#include "fp2_accel.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace esphome::aqara_fp2 {

// The Aqara FP2 tracks up to five simultaneous targets.
static constexpr uint8_t MAX_TARGETS = 5;
// Zone ids are 1-based indexes into the radar's 32-entry zone activation list.
static constexpr uint8_t MAX_ZONES = 30;

// Detection maps are 320-bit (20 rows x 16 columns) blobs.
static constexpr uint8_t GRID_SIZE = 40;
using GridMap = std::array<uint8_t, GRID_SIZE>;

// Largest attribute payload we send: zone map = attr id (2) + type (1) + length (2) + zone id (1) + grid.
static constexpr uint8_t MAX_CMD_DATA = 6 + GRID_SIZE;
// Holds ACKs, answers to radar queries and runtime writes. The configuration is not queued: it is
// generated one command at a time while the radar acknowledges each write.
static constexpr uint8_t CMD_QUEUE_SIZE = 8;
// Largest report payload we process (target list of MAX_TARGETS + framing). Longer frames are
// still checked and acknowledged, but not processed.
static constexpr uint8_t MAX_RX_PAYLOAD = 96;
// Frames declaring a longer payload are treated as corrupt and resynchronized immediately.
static constexpr uint16_t MAX_RX_FRAME_PAYLOAD = 2048;

#ifdef USE_SENSOR
// A sensor that is only published when its value changes. Unconfigured (null) sensors are ignored.
template<typename T> class DedupSensor {
 public:
  void set_sensor(sensor::Sensor *sens) { this->sens_ = sens; }
  sensor::Sensor *get_sensor() const { return this->sens_; }

  void publish(T value) {
    if (this->sens_ != nullptr && this->dedup_.next(value)) {
      this->sens_->publish_state(static_cast<float>(value));
    }
  }

  void publish_unknown() {
    if (this->sens_ != nullptr && this->dedup_.next_unknown()) {
      this->sens_->publish_state(NAN);
    }
  }

 protected:
  sensor::Sensor *sens_{nullptr};
  Deduplicator<T> dedup_;
};
#endif

// UART transport opcodes.
enum class OpCode : uint8_t {
  OP_CODE_RESPONSE = 0x01,  // device -> host response, or (2-byte payload) a radar query
  OP_CODE_WRITE = 0x02,     // host -> device attribute write
  OP_CODE_ACK = 0x03,       // acknowledgement
  OP_CODE_READ = 0x04,      // host -> device read request, or answer to a radar query
  OP_CODE_REPORT = 0x05,    // device -> host asynchronous report
};

// Radar attribute identifiers exchanged over the transport.
enum class AttrId : uint16_t {
  ATTR_ID_RADAR_SW_VERSION = 0x0102,             // heartbeat / firmware version
  ATTR_ID_MOTION_DETECT = 0x0103,                // global motion report
  ATTR_ID_PRESENCE_DETECT = 0x0104,              // global presence report
  ATTR_ID_MONITOR_MODE = 0x0105,                 // detection direction
  ATTR_ID_CLOSING_SETTING = 0x0106,              // proximity setting
  ATTR_ID_EDGE_MAP = 0x0107,                     // detection boundary map (40B)
  ATTR_ID_ENTRY_EXIT_MAP = 0x0109,               // enter/exit label map (40B)
  ATTR_ID_INTERFERENCE_MAP = 0x0110,             // interference source map (40B)
  ATTR_ID_PRESENCE_DETECT_SENSITIVITY = 0x0111,  // global presence sensitivity (1-3)
  ATTR_ID_LOCATION_REPORT_ENABLE = 0x0112,       // enable/disable target location reports
  ATTR_ID_ZONE_MAP = 0x0114,                     // zone area map (1B id + 40B)
  ATTR_ID_DETECT_ZONE_MOTION = 0x0115,           // per-zone motion report
  ATTR_ID_WORK_MODE = 0x0116,                    // enables presence/motion reports
  ATTR_ID_LOCATION_TRACKING_DATA = 0x0117,       // per-target location report
  ATTR_ID_ANGLE_SENSOR_DATA = 0x0120,            // radar query: mounting tilt angle
  ATTR_ID_LEFT_RIGHT_REVERSE = 0x0122,           // left/right swap
  ATTR_ID_TEMPERATURE = 0x0128,                  // radar temperature report
  ATTR_ID_THERMO_EN = 0x0138,
  ATTR_ID_THERMO_DATA = 0x0141,
  ATTR_ID_ZONE_PRESENCE = 0x0142,     // per-zone presence report
  ATTR_ID_DEVICE_DIRECTION = 0x0143,  // radar query: mounting orientation
  ATTR_ID_ZONE_SENSITIVITY = 0x0151,  // per-zone sensitivity
  ATTR_ID_ZONE_CLOSE_AWAY_ENABLE = 0x0153,
  ATTR_ID_PEOPLE_COUNT_REPORT_ENABLE = 0x0158,
  ATTR_ID_PEOPLE_NUMBER_ENABLE = 0x0162,
  ATTR_ID_TARGET_TYPE_ENABLE = 0x0163,  // AI person detection
  ATTR_ID_WALL_CORNER_POS = 0x0170,     // mounting position
  ATTR_ID_DWELL_TIME_ENABLE = 0x0172,
  ATTR_ID_WALK_DISTANCE_ENABLE = 0x0173,
  ATTR_ID_ZONE_ACTIVATION_LIST = 0x0202,  // active zone list (32B)
};

// Attribute data-type tags used inside a payload.
enum class DataType : uint8_t {
  DATA_TYPE_UINT8 = 0x00,
  DATA_TYPE_UINT16 = 0x01,
  DATA_TYPE_UINT32 = 0x02,
  DATA_TYPE_VOID = 0x03,
  DATA_TYPE_BOOL = 0x04,
  DATA_TYPE_STRING = 0x05,
  DATA_TYPE_BINARY = 0x06,
};

// An outgoing command. The transport frame (sync, sequence, checksums) is built at send time;
// only the attribute payload is stored here.
struct FP2Command {
  OpCode type;
  AttrId attr_id;
  uint8_t data[MAX_CMD_DATA];
  uint8_t data_len;
};

// Motion derived from the radar's movement events: on with an event, off on leave, when the
// presence clears, or after the motion timeout without events.
struct MotionTimer {
  uint32_t last_event_ms{0};
  bool active{false};
};

// A user-defined detection zone. Its radar zone id is its index + 1.
struct FP2Zone {
  GridMap grid;
  uint8_t sensitivity;  // 1=low, 2=medium, 3=high
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *presence{nullptr};
  binary_sensor::BinarySensor *motion{nullptr};
  MotionTimer motion_timer{};
#endif
};

class FP2Component;

#ifdef USE_SWITCH
// Switch that enables or disables live target-location reporting on the radar.
class LocationReportSwitch : public switch_::Switch, public Parented<FP2Component> {
 protected:
  void write_state(bool state) override;
};
#endif

#ifdef USE_BUTTON
// Button that resets the radar (or, without a reset pin, reconfigures it on its next heartbeat).
class RestartRadarButton : public button::Button, public Parented<FP2Component> {
 protected:
  void press_action() override;
};
#endif

class FP2Component : public Component, public uart::UARTDevice {
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(presence)
  SUB_BINARY_SENSOR(motion)
  SUB_BINARY_SENSOR(target)
#endif
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(radar_version)
#endif
#ifdef USE_SWITCH
  SUB_SWITCH(location_report)
#endif
#ifdef USE_BUTTON
  SUB_BUTTON(restart_radar)
#endif

 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_mounting_position(uint8_t position) { this->mounting_position_ = position; }
  void set_left_right_reverse(bool reverse) { this->left_right_reverse_ = reverse; }
  void set_presence_sensitivity(uint8_t sensitivity) { this->presence_sensitivity_ = sensitivity; }
  void set_radar_timeout(uint32_t timeout_ms) { this->radar_timeout_ms_ = timeout_ms; }
  void set_motion_timeout(uint32_t timeout_ms) { this->motion_timeout_ms_ = timeout_ms; }
#ifdef USE_EVENT
  void set_movement_event(event::Event *evt) { this->movement_event_ = evt; }
#endif
#ifdef USE_AQARA_FP2_ACCEL
  void set_accel(FP2Accel *accel) { this->accel_ = accel; }
#endif

  void set_interference_grid(const GridMap &grid) {
    this->interference_grid_ = grid;
    this->has_interference_grid_ = true;
  }
  void set_exit_grid(const GridMap &grid) {
    this->exit_grid_ = grid;
    this->has_exit_grid_ = true;
  }
  void set_edge_grid(const GridMap &grid) {
    this->edge_grid_ = grid;
    this->has_edge_grid_ = true;
  }

  void init_zones(uint8_t count) { this->zones_.init(count); }
  void add_zone(const GridMap &grid, uint8_t sensitivity) { this->zones_.push_back({grid, sensitivity}); }

#ifdef USE_BINARY_SENSOR
  // Zone entities are attached by index (0-based) after add_zone() created the zones.
  void set_zone_presence_binary_sensor(uint8_t zone, binary_sensor::BinarySensor *sens) {
    this->zones_[zone].presence = sens;
  }
  void set_zone_motion_binary_sensor(uint8_t zone, binary_sensor::BinarySensor *sens) {
    this->zones_[zone].motion = sens;
  }
#endif

#ifdef USE_SENSOR
  void set_target_count_sensor(sensor::Sensor *s) { this->target_count_sensor_.set_sensor(s); }
  void set_radar_temperature_sensor(sensor::Sensor *s) { this->radar_temperature_sensor_.set_sensor(s); }
  void set_target_x_sensor(uint8_t target, sensor::Sensor *s) { this->target_x_sensors_[target].set_sensor(s); }
  void set_target_y_sensor(uint8_t target, sensor::Sensor *s) { this->target_y_sensors_[target].set_sensor(s); }
  void set_target_z_sensor(uint8_t target, sensor::Sensor *s) { this->target_z_sensors_[target].set_sensor(s); }
  void set_target_speed_sensor(uint8_t target, sensor::Sensor *s) { this->target_speed_sensors_[target].set_sensor(s); }
#endif

  void set_location_reporting_enabled(bool enabled);
  void restart_radar();

 protected:
  enum class ConfigState : uint8_t {
    CONFIG_STATE_WAIT_HEARTBEAT,  // (re)configure on the next radar heartbeat
    CONFIG_STATE_SENDING,         // configuration commands in progress
    CONFIG_STATE_DONE,
  };

  enum class RxState : uint8_t {
    RX_STATE_SYNC,
    RX_STATE_HEADER,
    RX_STATE_PAYLOAD,
    RX_STATE_CRC_L,
    RX_STATE_CRC_H,
  };

  // Outgoing commands.
  bool enqueue_(OpCode type, AttrId attr_id, DataType data_type, const uint8_t *value, uint8_t value_len,
                bool front = false);
  void process_commands_(uint32_t now);
  void send_command_(const FP2Command &cmd, uint32_t now);
  void command_failed_();
  void revert_location_reporting_();

  // Configuration, generated one step at a time.
  void start_configuration_();
  uint8_t config_step_count_() const;
  bool build_config_command_(uint8_t step, FP2Command &cmd) const;

  // Radar watchdog.
  void perform_reset_();
  void check_radar_timeout_(uint32_t now);
  void publish_unknown_states_();

  // Frame decoding.
  void handle_incoming_byte_(uint8_t byte);
  void handle_frame_(OpCode op, AttrId attr_id);
  void handle_report_(AttrId attr_id);
  void handle_location_report_();
  void handle_radar_query_(AttrId attr_id);
  void handle_movement_(uint8_t value);
#ifdef USE_BINARY_SENSOR
  void set_motion_(binary_sensor::BinarySensor *sens, MotionTimer &timer, bool detected);
  void expire_motion_(uint32_t now);
#endif
  // Target sensors, target count and has_target: everything that comes from location reports.
  void publish_location_unknown_();

  GPIOPin *reset_pin_{nullptr};
#ifdef USE_AQARA_FP2_ACCEL
  FP2Accel *accel_{nullptr};
#endif

  uint8_t mounting_position_{0x01};
  bool left_right_reverse_{false};
  uint8_t presence_sensitivity_{2};
  uint32_t radar_timeout_ms_{0};
  uint32_t motion_timeout_ms_{0};
#ifdef USE_BINARY_SENSOR
  MotionTimer motion_timer_{};
#endif
#ifdef USE_EVENT
  event::Event *movement_event_{nullptr};
#endif

  GridMap interference_grid_{};
  GridMap exit_grid_{};
  GridMap edge_grid_{};
  bool has_interference_grid_{false};
  bool has_exit_grid_{false};
  bool has_edge_grid_{false};

  FixedVector<FP2Zone> zones_;

  ConfigState config_state_{ConfigState::CONFIG_STATE_WAIT_HEARTBEAT};
  uint8_t config_step_{0};
  // Without a location_report switch, target locations are always reported.
  bool location_reporting_active_{true};
  // Last state the radar acknowledged; a failed write returns to it.
  bool location_reporting_confirmed_{false};
  bool radar_lost_{false};
  bool reset_pending_{false};  // boot reset waits for the accelerometer
  uint32_t setup_ms_{0};
  uint32_t last_rx_ms_{0};

  // Outgoing ring buffer plus the write currently awaiting an ACK, held separately so the queue
  // head is always "next to send" and front inserts stay safe.
  std::array<FP2Command, CMD_QUEUE_SIZE> queue_{};
  uint8_t queue_head_{0};
  uint8_t queue_count_{0};
  FP2Command inflight_{};
  bool inflight_active_{false};
  bool inflight_is_config_{false};
  uint8_t inflight_sends_{0};
  uint32_t last_command_sent_ms_{0};
  uint8_t tx_seq_{0};

  // Incoming frame decoder.
  RxState rx_state_{RxState::RX_STATE_SYNC};
  std::array<uint8_t, 8> rx_header_{};  // sync, ver_h, ver_l, seq, opcode, len_h, len_l, header checksum
  uint8_t rx_header_pos_{0};
  uint16_t rx_len_{0};
  uint16_t rx_pos_{0};
  uint16_t rx_crc_{0};
  uint16_t rx_crc_calc_{0};  // running CRC over header and payload
  uint32_t rx_last_byte_ms_{0};
  uint8_t rx_payload_[MAX_RX_PAYLOAD];

#ifdef USE_SENSOR
  DedupSensor<uint8_t> target_count_sensor_{};
  DedupSensor<int16_t> radar_temperature_sensor_{};
  std::array<DedupSensor<int16_t>, MAX_TARGETS> target_x_sensors_{};
  std::array<DedupSensor<int16_t>, MAX_TARGETS> target_y_sensors_{};
  std::array<DedupSensor<int16_t>, MAX_TARGETS> target_z_sensors_{};
  std::array<DedupSensor<int16_t>, MAX_TARGETS> target_speed_sensors_{};
#endif
};

}  // namespace esphome::aqara_fp2
