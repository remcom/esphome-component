#include "aqara_fp2.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::aqara_fp2 {

static const char *const TAG = "aqara_fp2";

// Transport framing constants.
static constexpr uint8_t FRAME_SYNC = 0x55;
static constexpr uint8_t FRAME_VER_H = 0x00;
static constexpr uint8_t FRAME_VER_L = 0x01;
static constexpr uint8_t FRAME_HEADER_LEN = 7;  // sync, ver_h, ver_l, seq, opcode, len_h, len_l
// Each target record in a location report is a fixed 14 bytes.
static constexpr uint8_t TARGET_RECORD_LEN = 14;
// Orientation::ORIENTATION_UP, reported when no accelerometer is configured: an upright wall mount.
static constexpr uint8_t ORIENTATION_UP = 0;

static constexpr uint32_t ACK_TIMEOUT_MS = 500;
// A write is sent this many times before the radar is considered to have rejected it.
static constexpr uint8_t MAX_SENDS = 3;
static constexpr uint32_t RESET_PULSE_MS = 100;
// A frame takes about 1 ms at 890000 baud; a longer gap means bytes were lost.
static constexpr uint32_t RX_BYTE_TIMEOUT_MS = 50;
// Value bytes of a report shown in the debug log (enough for every non-blob report).
static constexpr size_t REPORT_LOG_BYTES = 8;

// Configuration steps, see build_config_command_().
static constexpr uint8_t ATTRIBUTE_STEPS = 15;
static constexpr uint8_t GRID_STEPS = 3;
static constexpr uint8_t ZONE_ACTIVATION_LIST_SIZE = 32;

PROGMEM_STRING_TABLE(MountingPositionStrings, "unknown", "wall", "left corner", "right corner");
PROGMEM_STRING_TABLE(SensitivityStrings, "unknown", "low", "medium", "high");

// Header checksum used by the transport: the low byte of ~(sum - 1) over the 7 header bytes.
static uint8_t header_checksum(const uint8_t *header) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < FRAME_HEADER_LEN; i++) {
    sum += header[i];
  }
  return static_cast<uint8_t>(~(sum - 1));
}

static void fill_command(FP2Command &cmd, OpCode type, AttrId attr_id, DataType data_type, const uint8_t *value,
                         uint8_t value_len) {
  const auto id = static_cast<uint16_t>(attr_id);
  cmd.type = type;
  cmd.attr_id = attr_id;
  cmd.data[0] = static_cast<uint8_t>(id >> 8);
  cmd.data[1] = static_cast<uint8_t>(id & 0xFF);
  cmd.data[2] = static_cast<uint8_t>(data_type);
  const uint8_t len = std::min<uint8_t>(value_len, MAX_CMD_DATA - 3);
  if (len > 0) {
    std::memcpy(cmd.data + 3, value, len);
  }
  cmd.data_len = 3 + len;
}

// Binary attributes carry a 16-bit length before their content.
static void fill_blob(FP2Command &cmd, AttrId attr_id, const uint8_t *content, uint8_t content_len) {
  uint8_t value[MAX_CMD_DATA - 3];
  content_len = std::min<uint8_t>(content_len, sizeof(value) - 2);
  value[0] = 0;
  value[1] = content_len;
  std::memcpy(value + 2, content, content_len);
  fill_command(cmd, OpCode::OP_CODE_WRITE, attr_id, DataType::DATA_TYPE_BINARY, value, content_len + 2);
}

static void fill_uint8(FP2Command &cmd, AttrId attr_id, DataType data_type, uint8_t value) {
  fill_command(cmd, OpCode::OP_CODE_WRITE, attr_id, data_type, &value, 1);
}

static void fill_uint16(FP2Command &cmd, AttrId attr_id, uint16_t value) {
  const uint8_t bytes[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
  fill_command(cmd, OpCode::OP_CODE_WRITE, attr_id, DataType::DATA_TYPE_UINT16, bytes, 2);
}

void FP2Component::setup() {
#ifdef USE_SWITCH
  if (this->location_report_switch_ != nullptr) {
    this->location_reporting_active_ =
        this->location_report_switch_->get_initial_state_with_restore_mode().value_or(false);
    this->location_report_switch_->publish_state(this->location_reporting_active_);
  }
#endif
  this->last_rx_ms_ = millis();
  this->perform_reset_();
}

void FP2Component::perform_reset_() {
  if (this->reset_pin_ == nullptr) {
    return;
  }
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(false);
  this->set_timeout(RESET_PULSE_MS, [this]() { this->reset_pin_->digital_write(true); });
}

void FP2Component::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->available() > 0) {
    this->rx_last_byte_ms_ = now;
    uint8_t byte;
    while (this->available() > 0 && this->read_byte(&byte)) {
      this->handle_incoming_byte_(byte);
    }
  } else if (this->rx_state_ != RxState::RX_STATE_SYNC && now - this->rx_last_byte_ms_ > RX_BYTE_TIMEOUT_MS) {
    // Drop a truncated frame instead of reading the next frame's bytes into it.
    ESP_LOGV(TAG, "Incomplete frame, resynchronizing");
    this->rx_state_ = RxState::RX_STATE_SYNC;
  }

  this->check_radar_timeout_(now);
  this->process_commands_(now);
}

// --- Configuration -------------------------------------------------------------------------

void FP2Component::start_configuration_() {
  ESP_LOGD(TAG, "Radar heartbeat, sending configuration");
  this->config_state_ = ConfigState::CONFIG_STATE_SENDING;
  this->config_step_ = 0;

  // The radar reports changes only, so start from its post-reset state. Location entities stay
  // unknown while location reporting is off.
#ifdef USE_SENSOR
  if (this->location_reporting_active_) {
    this->target_count_sensor_.publish(0);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto *sens : {this->presence_binary_sensor_, this->motion_binary_sensor_}) {
    if (sens != nullptr) {
      sens->publish_state(false);
    }
  }
  if (this->target_binary_sensor_ != nullptr && this->location_reporting_active_) {
    this->target_binary_sensor_->publish_state(false);
  }
  for (const auto &zone : this->zones_) {
    if (zone.presence != nullptr) {
      zone.presence->publish_state(false);
    }
    if (zone.motion != nullptr) {
      zone.motion->publish_state(false);
    }
  }
#endif
}

uint8_t FP2Component::config_step_count_() const {
  // Attributes, grids, map + sensitivity per zone, the activation list, close/away per zone.
  return static_cast<uint8_t>(ATTRIBUTE_STEPS + GRID_STEPS + 3 * this->zones_.size() + 1);
}

// Fills `cmd` with configuration step `step`. Returns false for a step that does not apply (an
// unconfigured grid), which is then skipped.
bool FP2Component::build_config_command_(uint8_t step, FP2Command &cmd) const {
  if (step < ATTRIBUTE_STEPS) {
    switch (step) {
      case 0:
        fill_uint8(cmd, AttrId::ATTR_ID_MONITOR_MODE, DataType::DATA_TYPE_UINT8, 0);
        break;
      case 1:
        fill_uint8(cmd, AttrId::ATTR_ID_LEFT_RIGHT_REVERSE, DataType::DATA_TYPE_UINT8,
                   this->left_right_reverse_ ? 2 : 0);
        break;
      case 2:
        fill_uint8(cmd, AttrId::ATTR_ID_PRESENCE_DETECT_SENSITIVITY, DataType::DATA_TYPE_UINT8,
                   this->presence_sensitivity_);
        break;
      case 3:
        fill_uint8(cmd, AttrId::ATTR_ID_CLOSING_SETTING, DataType::DATA_TYPE_UINT8, 1);
        break;
      case 4:
        fill_uint16(cmd, AttrId::ATTR_ID_ZONE_CLOSE_AWAY_ENABLE, 0x0001);
        break;
      case 5:
        fill_uint8(cmd, AttrId::ATTR_ID_PEOPLE_COUNT_REPORT_ENABLE, DataType::DATA_TYPE_BOOL, 1);
        break;
      case 6:
        fill_uint8(cmd, AttrId::ATTR_ID_PEOPLE_NUMBER_ENABLE, DataType::DATA_TYPE_BOOL, 1);
        break;
      case 7:
        fill_uint8(cmd, AttrId::ATTR_ID_TARGET_TYPE_ENABLE, DataType::DATA_TYPE_BOOL, 1);
        break;
      case 8:
        fill_uint8(cmd, AttrId::ATTR_ID_WALL_CORNER_POS, DataType::DATA_TYPE_UINT8, this->mounting_position_);
        break;
      case 9:
        fill_uint8(cmd, AttrId::ATTR_ID_DWELL_TIME_ENABLE, DataType::DATA_TYPE_UINT8, 0);
        break;
      case 10:
        fill_uint8(cmd, AttrId::ATTR_ID_WALK_DISTANCE_ENABLE, DataType::DATA_TYPE_UINT8, 0);
        break;
      // Steps 11 and 12 follow the stock firmware's start-up sequence; nothing here uses the
      // thermal reports they enable (larger ones are acknowledged without being processed).
      case 11:
        fill_uint8(cmd, AttrId::ATTR_ID_THERMO_EN, DataType::DATA_TYPE_BOOL, 1);
        break;
      case 12:
        fill_uint8(cmd, AttrId::ATTR_ID_THERMO_DATA, DataType::DATA_TYPE_UINT8, 1);
        break;
      case 13:
        fill_uint8(cmd, AttrId::ATTR_ID_LOCATION_REPORT_ENABLE, DataType::DATA_TYPE_UINT8,
                   this->location_reporting_active_ ? 1 : 0);
        break;
      // The radar sends no presence or motion reports until the work mode is written; the stock
      // firmware writes 3.
      default:
        fill_uint8(cmd, AttrId::ATTR_ID_WORK_MODE, DataType::DATA_TYPE_UINT8, 3);
        break;
    }
    return true;
  }
  step -= ATTRIBUTE_STEPS;

  if (step < GRID_STEPS) {
    if (step == 0 && this->has_interference_grid_) {
      fill_blob(cmd, AttrId::ATTR_ID_INTERFERENCE_MAP, this->interference_grid_.data(), GRID_SIZE);
      return true;
    }
    if (step == 1 && this->has_exit_grid_) {
      fill_blob(cmd, AttrId::ATTR_ID_ENTRY_EXIT_MAP, this->exit_grid_.data(), GRID_SIZE);
      return true;
    }
    if (step == 2 && this->has_edge_grid_) {
      fill_blob(cmd, AttrId::ATTR_ID_EDGE_MAP, this->edge_grid_.data(), GRID_SIZE);
      return true;
    }
    return false;
  }
  step -= GRID_STEPS;

  const uint8_t zone_count = this->zones_.size();
  if (step < 2 * zone_count) {
    const uint8_t index = step / 2;
    const FP2Zone &zone = this->zones_[index];
    const uint8_t zone_id = index + 1;
    if (step % 2 == 0) {
      uint8_t content[1 + GRID_SIZE];
      content[0] = zone_id;
      std::copy(zone.grid.begin(), zone.grid.end(), content + 1);
      fill_blob(cmd, AttrId::ATTR_ID_ZONE_MAP, content, sizeof(content));
    } else {
      fill_uint16(cmd, AttrId::ATTR_ID_ZONE_SENSITIVITY, (zone_id << 8) | zone.sensitivity);
    }
    return true;
  }
  step -= 2 * zone_count;

  if (step == 0) {
    uint8_t activations[ZONE_ACTIVATION_LIST_SIZE] = {0};
    for (uint8_t zone_id = 1; zone_id <= zone_count; zone_id++) {
      activations[zone_id] = zone_id;
    }
    fill_blob(cmd, AttrId::ATTR_ID_ZONE_ACTIVATION_LIST, activations, sizeof(activations));
    return true;
  }
  step -= 1;

  fill_uint16(cmd, AttrId::ATTR_ID_ZONE_CLOSE_AWAY_ENABLE, ((step + 1) << 8) | 1);
  return true;
}

// --- Outgoing commands ---------------------------------------------------------------------

bool FP2Component::enqueue_(OpCode type, AttrId attr_id, DataType data_type, const uint8_t *value, uint8_t value_len,
                            bool front) {
  if (this->queue_count_ >= CMD_QUEUE_SIZE) {
    ESP_LOGW(TAG, "Command queue full, dropping 0x%04X", static_cast<uint16_t>(attr_id));
    this->status_set_warning(LOG_STR("Command queue full"));
    return false;
  }

  uint8_t index;
  if (front) {
    this->queue_head_ = (this->queue_head_ + CMD_QUEUE_SIZE - 1) % CMD_QUEUE_SIZE;
    index = this->queue_head_;
  } else {
    index = (this->queue_head_ + this->queue_count_) % CMD_QUEUE_SIZE;
  }
  this->queue_count_++;
  fill_command(this->queue_[index], type, attr_id, data_type, value, value_len);
  return true;
}

void FP2Component::process_commands_(uint32_t now) {
  // A write is in flight: wait for its ACK and resend on timeout.
  if (this->inflight_active_) {
    if (now - this->last_command_sent_ms_ <= ACK_TIMEOUT_MS) {
      return;
    }
    if (this->inflight_sends_ >= MAX_SENDS) {
      this->command_failed_();
      return;
    }
    ESP_LOGD(TAG, "No ACK for 0x%04X, resending", static_cast<uint16_t>(this->inflight_.attr_id));
    this->send_command_(this->inflight_, now);
    return;
  }

  // Queued commands go first; ACKs and answers are not acknowledged, so keep sending until a write.
  while (this->queue_count_ > 0) {
    this->inflight_ = this->queue_[this->queue_head_];
    this->queue_head_ = (this->queue_head_ + 1) % CMD_QUEUE_SIZE;
    this->queue_count_--;
    this->inflight_is_config_ = false;
    this->inflight_sends_ = 0;
    this->send_command_(this->inflight_, now);
    if (this->inflight_active_) {
      return;
    }
  }

  if (this->config_state_ != ConfigState::CONFIG_STATE_SENDING) {
    return;
  }
  const uint8_t step_count = this->config_step_count_();
  while (this->config_step_ < step_count) {
    if (this->build_config_command_(this->config_step_++, this->inflight_)) {
      this->inflight_is_config_ = true;
      this->inflight_sends_ = 0;
      this->send_command_(this->inflight_, now);
      return;
    }
  }
  // The last configuration write has been acknowledged.
  this->config_state_ = ConfigState::CONFIG_STATE_DONE;
  ESP_LOGD(TAG, "Radar configured");
  this->status_clear_warning();
}

void FP2Component::send_command_(const FP2Command &cmd, uint32_t now) {
  uint8_t frame[FRAME_HEADER_LEN + 1 + MAX_CMD_DATA + 2];
  uint8_t pos = 0;
  frame[pos++] = FRAME_SYNC;
  frame[pos++] = FRAME_VER_H;
  frame[pos++] = FRAME_VER_L;
  frame[pos++] = this->tx_seq_++;
  frame[pos++] = static_cast<uint8_t>(cmd.type);
  frame[pos++] = 0;  // length high byte: commands are always shorter than 256 bytes
  frame[pos++] = cmd.data_len;
  frame[pos] = header_checksum(frame);
  pos++;
  std::memcpy(frame + pos, cmd.data, cmd.data_len);
  pos += cmd.data_len;

  const uint16_t crc = crc16(frame, pos);
  frame[pos++] = static_cast<uint8_t>(crc & 0xFF);
  frame[pos++] = static_cast<uint8_t>(crc >> 8);

  this->write_array(frame, pos);
  this->last_command_sent_ms_ = now;
  this->inflight_sends_++;
  // Only attribute writes are acknowledged; everything else is fire-and-forget.
  this->inflight_active_ = cmd.type == OpCode::OP_CODE_WRITE;
}

void FP2Component::command_failed_() {
  this->inflight_active_ = false;
  ESP_LOGW(TAG, "Radar did not acknowledge 0x%04X", static_cast<uint16_t>(this->inflight_.attr_id));
  if (this->inflight_is_config_) {
    // Never continue with a half-applied configuration: start over on the next heartbeat.
    this->config_state_ = ConfigState::CONFIG_STATE_WAIT_HEARTBEAT;
    this->status_set_warning(LOG_STR("Radar configuration failed, retrying on next heartbeat"));
  } else {
    this->status_set_warning(LOG_STR("Radar did not acknowledge a command"));
    if (this->inflight_.attr_id == AttrId::ATTR_ID_LOCATION_REPORT_ENABLE) {
      this->revert_location_reporting_();
    }
  }
}

// A location report write was dropped or not acknowledged: go back to the last state the radar
// acknowledged, so the switch, this component and the radar agree.
void FP2Component::revert_location_reporting_() {
  this->location_reporting_active_ = this->location_reporting_confirmed_;
#ifdef USE_SWITCH
  if (this->location_report_switch_ != nullptr) {
    this->location_report_switch_->publish_state(this->location_reporting_confirmed_);
  }
#endif
  // Reports may have been published while the write was being retried.
  if (!this->location_reporting_confirmed_) {
    this->publish_location_unknown_();
  }
}

// --- Radar watchdog ------------------------------------------------------------------------

void FP2Component::check_radar_timeout_(uint32_t now) {
  if (this->radar_timeout_ms_ == 0 || this->radar_lost_ || now - this->last_rx_ms_ <= this->radar_timeout_ms_) {
    return;
  }
  this->radar_lost_ = true;
  this->config_state_ = ConfigState::CONFIG_STATE_WAIT_HEARTBEAT;
  this->inflight_active_ = false;
  this->queue_count_ = 0;
  this->status_set_warning(LOG_STR("No data from radar, reconfiguring on next heartbeat"));
  this->publish_unknown_states_();
}

void FP2Component::publish_unknown_states_() {
#ifdef USE_BINARY_SENSOR
  for (auto *sens : {this->presence_binary_sensor_, this->motion_binary_sensor_}) {
    if (sens != nullptr) {
      sens->invalidate_state();
    }
  }
  for (const auto &zone : this->zones_) {
    if (zone.presence != nullptr) {
      zone.presence->invalidate_state();
    }
    if (zone.motion != nullptr) {
      zone.motion->invalidate_state();
    }
  }
#endif
#ifdef USE_SENSOR
  this->radar_temperature_sensor_.publish_unknown();
#endif
  this->publish_location_unknown_();
}

void FP2Component::publish_location_unknown_() {
#ifdef USE_SENSOR
  for (uint8_t i = 0; i < MAX_TARGETS; i++) {
    this->target_x_sensors_[i].publish_unknown();
    this->target_y_sensors_[i].publish_unknown();
    this->target_z_sensors_[i].publish_unknown();
    this->target_speed_sensors_[i].publish_unknown();
  }
  this->target_count_sensor_.publish_unknown();
#endif
#ifdef USE_BINARY_SENSOR
  if (this->target_binary_sensor_ != nullptr) {
    this->target_binary_sensor_->invalidate_state();
  }
#endif
}

// --- Incoming frame decoder ----------------------------------------------------------------

void FP2Component::handle_incoming_byte_(uint8_t byte) {
  switch (this->rx_state_) {
    case RxState::RX_STATE_SYNC:
      if (byte == FRAME_SYNC) {
        this->rx_header_[0] = byte;
        this->rx_header_pos_ = 1;
        this->rx_state_ = RxState::RX_STATE_HEADER;
      }
      return;
    case RxState::RX_STATE_HEADER: {
      const uint8_t pos = this->rx_header_pos_++;
      this->rx_header_[pos] = byte;
      if ((pos == 1 && byte != FRAME_VER_H) || (pos == 2 && byte != FRAME_VER_L)) {
        this->rx_state_ = RxState::RX_STATE_SYNC;
        return;
      }
      if (pos < FRAME_HEADER_LEN) {
        return;
      }
      if (byte != header_checksum(this->rx_header_.data())) {
        ESP_LOGV(TAG, "Header checksum mismatch");
        this->rx_state_ = RxState::RX_STATE_SYNC;
        return;
      }
      // A frame too long to process is still read to its end (and its CRC checked) rather than
      // resynchronizing in the middle of it; only an implausible length resynchronizes at once.
      this->rx_len_ = encode_uint16(this->rx_header_[5], this->rx_header_[6]);
      if (this->rx_len_ > MAX_RX_FRAME_PAYLOAD) {
        ESP_LOGW(TAG, "Implausible frame length %u, resynchronizing", this->rx_len_);
        this->rx_state_ = RxState::RX_STATE_SYNC;
        return;
      }
      this->rx_pos_ = 0;
      this->rx_crc_calc_ = crc16(this->rx_header_.data(), this->rx_header_.size());
      this->rx_state_ = this->rx_len_ == 0 ? RxState::RX_STATE_CRC_L : RxState::RX_STATE_PAYLOAD;
      return;
    }
    case RxState::RX_STATE_PAYLOAD:
      if (this->rx_pos_ < MAX_RX_PAYLOAD) {
        this->rx_payload_[this->rx_pos_] = byte;
      }
      this->rx_crc_calc_ = crc16(&byte, 1, this->rx_crc_calc_);
      if (++this->rx_pos_ >= this->rx_len_) {
        this->rx_state_ = RxState::RX_STATE_CRC_L;
      }
      return;
    case RxState::RX_STATE_CRC_L:
      this->rx_crc_ = byte;
      this->rx_state_ = RxState::RX_STATE_CRC_H;
      return;
    case RxState::RX_STATE_CRC_H: {
      this->rx_crc_ |= static_cast<uint16_t>(byte) << 8;
      this->rx_state_ = RxState::RX_STATE_SYNC;
      if (this->rx_crc_calc_ != this->rx_crc_) {
        ESP_LOGW(TAG, "Frame CRC mismatch");
        return;
      }
      this->last_rx_ms_ = App.get_loop_component_start_time();
      this->radar_lost_ = false;
      if (this->rx_len_ < 2) {
        return;
      }
      const auto op = static_cast<OpCode>(this->rx_header_[4]);
      const auto attr_id = static_cast<AttrId>(encode_uint16(this->rx_payload_[0], this->rx_payload_[1]));
      if (this->rx_len_ > MAX_RX_PAYLOAD) {
        // Not processed, but a report must still be acknowledged or the radar keeps resending it.
        ESP_LOGD(TAG, "Ignoring 0x%04X: %u bytes is more than %u", static_cast<uint16_t>(attr_id), this->rx_len_,
                 MAX_RX_PAYLOAD);
        if (op == OpCode::OP_CODE_REPORT) {
          this->enqueue_(OpCode::OP_CODE_ACK, attr_id, DataType::DATA_TYPE_VOID, nullptr, 0, /*front=*/true);
        }
        return;
      }
      this->handle_frame_(op, attr_id);
      return;
    }
  }
}

void FP2Component::handle_frame_(OpCode op, AttrId attr_id) {
  switch (op) {
    case OpCode::OP_CODE_ACK:
      if (this->inflight_active_ && this->inflight_.attr_id == attr_id) {
        this->inflight_active_ = false;
        if (attr_id == AttrId::ATTR_ID_LOCATION_REPORT_ENABLE) {
          this->location_reporting_confirmed_ = this->inflight_.data[3] != 0;
        }
        if (!this->inflight_is_config_ && this->config_state_ == ConfigState::CONFIG_STATE_DONE) {
          this->status_clear_warning();
        }
      }
      return;
    case OpCode::OP_CODE_REPORT:
      this->handle_report_(attr_id);
      return;
    case OpCode::OP_CODE_RESPONSE:
      // A response carrying only an attribute id is the radar asking us for a value.
      if (this->rx_len_ == 2) {
        this->handle_radar_query_(attr_id);
      }
      return;
    default:
      return;
  }
}

void FP2Component::handle_report_(AttrId attr_id) {
  const uint8_t *payload = this->rx_payload_;
  const uint16_t len = this->rx_len_;

  // Every report except the heartbeat must be acknowledged; ACKs jump the queue.
  if (attr_id == AttrId::ATTR_ID_RADAR_SW_VERSION) {
    if (this->config_state_ == ConfigState::CONFIG_STATE_WAIT_HEARTBEAT) {
      this->start_configuration_();
    }
#ifdef USE_TEXT_SENSOR
    if (this->radar_version_text_sensor_ != nullptr && len == 4 && payload[2] == 0x00) {
      char buf[4];
      snprintf(buf, sizeof(buf), "%u", payload[3]);
      if (this->radar_version_text_sensor_->state != buf) {
        this->radar_version_text_sensor_->publish_state(buf);
      }
    }
#endif
    return;
  }
  this->enqueue_(OpCode::OP_CODE_ACK, attr_id, DataType::DATA_TYPE_VOID, nullptr, 0, /*front=*/true);

  // Raw report (type tag and value), except the frequent location reports: the value formats were
  // reverse-engineered and are not all confirmed on hardware.
  if (attr_id != AttrId::ATTR_ID_LOCATION_TRACKING_DATA) {
    char hex[format_hex_pretty_size(REPORT_LOG_BYTES)];
    ESP_LOGD(TAG, "Report 0x%04X: %s", static_cast<uint16_t>(attr_id), format_hex_pretty_to(hex, payload + 2, len - 2));
  }

  switch (attr_id) {
#ifdef USE_BINARY_SENSOR
    case AttrId::ATTR_ID_PRESENCE_DETECT:
      if (this->presence_binary_sensor_ != nullptr && len == 4 && payload[2] == 0x00) {
        this->presence_binary_sensor_->publish_state(payload[3] != 0);
      }
      return;
    case AttrId::ATTR_ID_MOTION_DETECT:
      if (this->motion_binary_sensor_ != nullptr && len == 4 && payload[2] == 0x00) {
        this->motion_binary_sensor_->publish_state(payload[3] != 0);
      }
      return;
    case AttrId::ATTR_ID_ZONE_PRESENCE:
    case AttrId::ATTR_ID_DETECT_ZONE_MOTION: {
      // Payload: [attr 2][type 0x01][zone id][state]
      if (len < 5 || payload[2] != 0x01 || payload[3] == 0 || payload[3] > this->zones_.size()) {
        return;
      }
      const FP2Zone &zone = this->zones_[payload[3] - 1];
      auto *sens = attr_id == AttrId::ATTR_ID_ZONE_PRESENCE ? zone.presence : zone.motion;
      if (sens != nullptr) {
        sens->publish_state(payload[4] != 0);
      }
      return;
    }
#endif
#ifdef USE_SENSOR
    case AttrId::ATTR_ID_TEMPERATURE:
      if (len == 5 && payload[2] == 0x01) {
        this->radar_temperature_sensor_.publish(static_cast<int16_t>(encode_uint16(payload[3], payload[4])));
      }
      return;
#endif
    case AttrId::ATTR_ID_LOCATION_TRACKING_DATA:
      this->handle_location_report_();
      return;
    default:
      return;
  }
}

void FP2Component::handle_location_report_() {
  if (!this->location_reporting_active_) {
    return;
  }
  const uint8_t *payload = this->rx_payload_;
  // Payload: [attr 2][type 0x06][blob len 2][count 1][target record 14]...
  if (this->rx_len_ < 6 || payload[2] != static_cast<uint8_t>(DataType::DATA_TYPE_BINARY)) {
    return;
  }
  const uint8_t count = std::min(payload[5], MAX_TARGETS);

#ifdef USE_SENSOR
  for (uint8_t i = 0; i < MAX_TARGETS; i++) {
    const uint16_t offset = 6 + static_cast<uint16_t>(i) * TARGET_RECORD_LEN;
    if (i < count && offset + TARGET_RECORD_LEN <= this->rx_len_) {
      // Record layout: id(1) x(2) y(2) z(2) velocity(2) snr(2) classifier(1) posture(1) active(1).
      const uint8_t *record = payload + offset;
      this->target_x_sensors_[i].publish(static_cast<int16_t>(encode_uint16(record[1], record[2])));
      this->target_y_sensors_[i].publish(static_cast<int16_t>(encode_uint16(record[3], record[4])));
      this->target_z_sensors_[i].publish(static_cast<int16_t>(encode_uint16(record[5], record[6])));
      this->target_speed_sensors_[i].publish(static_cast<int16_t>(encode_uint16(record[7], record[8])));
    } else {
      this->target_x_sensors_[i].publish_unknown();
      this->target_y_sensors_[i].publish_unknown();
      this->target_z_sensors_[i].publish_unknown();
      this->target_speed_sensors_[i].publish_unknown();
    }
  }
  this->target_count_sensor_.publish(count);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->target_binary_sensor_ != nullptr) {
    this->target_binary_sensor_->publish_state(count > 0);
  }
#endif
}

void FP2Component::handle_radar_query_(AttrId attr_id) {
  uint8_t value;
  switch (attr_id) {
    case AttrId::ATTR_ID_DEVICE_DIRECTION:
      value = ORIENTATION_UP;
#ifdef USE_AQARA_FP2_ACCEL
      if (this->accel_ != nullptr) {
        value = static_cast<uint8_t>(this->accel_->get_orientation());
      }
#endif
      break;
    case AttrId::ATTR_ID_ANGLE_SENSOR_DATA:
      value = 0;
#ifdef USE_AQARA_FP2_ACCEL
      if (this->accel_ != nullptr) {
        value = this->accel_->get_output_angle_z();
      }
#endif
      break;
    default:
      ESP_LOGD(TAG, "Unhandled radar query 0x%04X", static_cast<uint16_t>(attr_id));
      return;
  }
  // Answer before any pending configuration writes.
  this->enqueue_(OpCode::OP_CODE_READ, attr_id, DataType::DATA_TYPE_UINT8, &value, 1, /*front=*/true);
}

void FP2Component::set_location_reporting_enabled(bool enabled) {
  this->location_reporting_active_ = enabled;
  // While waiting for a heartbeat the state is sent with the configuration.
  if (this->config_state_ != ConfigState::CONFIG_STATE_WAIT_HEARTBEAT) {
    const uint8_t value = enabled ? 1 : 0;
    if (!this->enqueue_(OpCode::OP_CODE_WRITE, AttrId::ATTR_ID_LOCATION_REPORT_ENABLE, DataType::DATA_TYPE_UINT8,
                        &value, 1)) {
      this->revert_location_reporting_();
      return;
    }
  }
  if (!enabled) {
    this->publish_location_unknown_();
  }
}

void FP2Component::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Aqara FP2:\n"
                "  Mounting position: %s\n"
                "  Left/right reverse: %s\n"
                "  Presence sensitivity: %s\n"
                "  Radar timeout: %" PRIu32 " ms",
                LOG_STR_ARG(MountingPositionStrings::get_log_str(this->mounting_position_, 0)),
                YESNO(this->left_right_reverse_),
                LOG_STR_ARG(SensitivityStrings::get_log_str(this->presence_sensitivity_, 0)), this->radar_timeout_ms_);
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  for (size_t i = 0; i < this->zones_.size(); i++) {
    const FP2Zone &zone = this->zones_[i];
    ESP_LOGCONFIG(TAG, "  Zone %u: sensitivity %s", static_cast<unsigned>(i + 1),
                  LOG_STR_ARG(SensitivityStrings::get_log_str(zone.sensitivity, 0)));
#ifdef USE_BINARY_SENSOR
    LOG_BINARY_SENSOR("    ", "Presence", zone.presence);
    LOG_BINARY_SENSOR("    ", "Motion", zone.motion);
#endif
  }
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Presence", this->presence_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Motion", this->motion_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Has target", this->target_binary_sensor_);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Target count", this->target_count_sensor_.get_sensor());
  LOG_SENSOR("  ", "Radar temperature", this->radar_temperature_sensor_.get_sensor());
  for (uint8_t i = 0; i < MAX_TARGETS; i++) {
    LOG_SENSOR("  ", "Target X", this->target_x_sensors_[i].get_sensor());
    LOG_SENSOR("  ", "Target Y", this->target_y_sensors_[i].get_sensor());
    LOG_SENSOR("  ", "Target Z", this->target_z_sensors_[i].get_sensor());
    LOG_SENSOR("  ", "Target speed", this->target_speed_sensors_[i].get_sensor());
  }
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Radar version", this->radar_version_text_sensor_);
#endif
#ifdef USE_SWITCH
  LOG_SWITCH("  ", "Location report", this->location_report_switch_);
#endif
}

#ifdef USE_SWITCH
void LocationReportSwitch::write_state(bool state) {
  // Published first: the parent publishes the old state again if the radar does not take it.
  this->publish_state(state);
  this->parent_->set_location_reporting_enabled(state);
}
#endif

}  // namespace esphome::aqara_fp2
