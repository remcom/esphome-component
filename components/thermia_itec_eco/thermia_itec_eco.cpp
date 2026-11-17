#include "thermia_itec_eco.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <utility>

namespace esphome::thermia_itec_eco {

static const char *const TAG = "thermia_itec_eco";

// Slaves on the bus. The display panel is the only master.
static constexpr uint8_t SLAVE_CONTROLLER = 0x02;    // indoor I/O board: temperatures, outputs
static constexpr uint8_t SLAVE_ROOM_SENSOR = 0x0A;   // accessory slot: Modbus room sensor (086U9563)
static constexpr uint8_t SLAVE_SETTINGS = 0x0F;      // settings pages published by the display
static constexpr uint8_t SLAVE_OUTDOOR_UNIT = 0x1E;  // outdoor unit (via the COMM KIT)

static constexpr uint8_t FUNCTION_READ_HOLDING = 0x03;
static constexpr uint8_t FUNCTION_READ_INPUT = 0x04;
static constexpr uint8_t FUNCTION_WRITE_MULTIPLE = 0x10;
static constexpr uint8_t FUNCTION_READ_WRITE_MULTIPLE = 0x17;
static constexpr uint8_t FUNCTION_EXCEPTION = 0x80;

// Controller (0x02), read by the display: whole °C, NOT_CONNECTED for an absent probe.
static constexpr uint16_t REG_SUPPLY_TEMPERATURE = 0xA7F8;
static constexpr uint16_t REG_HOT_WATER_TOP_TEMPERATURE = 0xA7FB;
static constexpr uint16_t REG_HOT_WATER_TEMPERATURE = 0xA7FC;
static constexpr uint16_t REG_OUTDOOR_TEMPERATURE = 0xA802;
static constexpr int16_t NOT_CONNECTED = -1000;
// Controller (0x02), written by the display: outputs (mapped with the display's manual test).
static constexpr uint16_t REG_OUTPUTS = 0xA80C;
static constexpr uint16_t OUTPUT_HOT_WATER_VALVE = 1 << 0;
static constexpr uint16_t OUTPUT_EXTERNAL_AUX_HEATER = 1 << 3;
static constexpr uint16_t OUTPUT_IMMERSION_HEATER_1 = 1 << 7;
static constexpr uint16_t REG_OUTPUTS_2 = 0xA80D;
static constexpr uint16_t OUTPUT_2_IMMERSION_HEATER_2 = 1 << 4;
static constexpr uint16_t REG_CIRCULATION_PUMP_SPEED = 0xA80F;  // %, 5 = stopped

// Outdoor unit (0x1E), read by the display (FC04): tenths unless noted.
static constexpr uint16_t REG_CONDENSER_INLET = 0x0000;
static constexpr uint16_t REG_CONDENSER_OUTLET = 0x0001;
static constexpr uint16_t REG_HIGH_PRESSURE = 0x0008;           // 0.1 bar
static constexpr uint16_t REG_COMPRESSOR_FREQUENCY = 0x000B;    // Hz
static constexpr uint16_t REG_COMPRESSOR_CURRENT = 0x000D;      // 0.1 A
static constexpr uint16_t REG_OUTDOOR_FAN_SPEED = 0x000E;       // rpm
static constexpr uint16_t REG_EXPANSION_VALVE_POSITION = 0x0010;  // steps
static constexpr uint16_t NOT_AVAILABLE = 0xFFFF;
static constexpr uint16_t REG_UNIT_STATE = 0x0014;
static constexpr uint16_t UNIT_STATE_COMPRESSOR = 1 << 0;
static constexpr uint16_t UNIT_STATE_OUTDOOR_FAN = 1 << 2;
static constexpr uint16_t UNIT_STATE_CIRCULATION_PUMP = 1 << 3;
static constexpr uint16_t REG_UNIT_STATUS = 0x0015;
static constexpr uint16_t UNIT_STATUS_DEFROST = 1 << 2;
// Outdoor unit (0x1E), written by the display (FC16).
static constexpr uint16_t REG_TARGET_SUPPLY_TEMPERATURE = 0x0001;  // 0.1 °C
static constexpr uint16_t REG_MODE_REQUEST = 0x0004;               // 1 heating, 2 hot water, 3 cooling
static constexpr uint16_t MODE_HEATING = 1;
static constexpr uint16_t MODE_HOT_WATER = 2;
static constexpr uint16_t MODE_COOLING = 3;

// Room sensor slot (0x0A): the display reads three words and writes three in one FC23 poll.
static constexpr uint16_t REG_ROOM_TEMPERATURE = 0xB3B0;  // 0.1 °C, from the sensor
static constexpr uint16_t ROOM_SENSOR_READ_COUNT = 3;     // temperature, setpoint request, top-up request
static constexpr uint16_t REG_ROOM_OUTDOOR = 0xB3C4;      // pushed by the display (unused here)
static constexpr uint16_t REG_ROOM_SETPOINT = 0xB3C5;     // °C, the controller's room setpoint
static constexpr uint16_t REG_ROOM_TOP_UP_STATUS = 0xB3C6;  // 2 while a hot water top-up runs
static constexpr uint16_t ROOM_TOP_UP_ACTIVE = 2;

// Settings (0x0F), written by the display: the HEATING menu page at 1000.
static constexpr uint16_t REG_HEATING_CURVE = 1000;
static constexpr uint16_t REG_HEATING_CURVE_MIN = 1001;
static constexpr uint16_t REG_HEATING_CURVE_MAX = 1002;
static constexpr uint16_t REG_CURVE_CORRECTION_PLUS_5 = 1003;
static constexpr uint16_t REG_CURVE_CORRECTION_0 = 1004;
static constexpr uint16_t REG_CURVE_CORRECTION_MINUS_5 = 1005;
static constexpr uint16_t REG_HEATING_STOP_TEMPERATURE = 1006;
static constexpr uint16_t REG_SETBACK_TEMPERATURE = 1007;
static constexpr uint16_t REG_ROOM_FACTOR = 1008;
static constexpr uint16_t REG_HIGH_POWER = 1011;

// A partial frame that has not grown for this long is noise: resynchronise.
static constexpr uint32_t RX_STALE_MS = 50;
// Modbus RTU frame gap at 9600 baud, 11 bits per character: 3.5 characters ≈ 4 ms.
static constexpr uint32_t FRAME_GAP_US = 4100;
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
// A setpoint request the controller has not adopted after this many replies is dropped.
static constexpr uint8_t MAX_SETPOINT_REPLIES = 5;
// Accepted room temperature input, °C.
static constexpr float MIN_ROOM_TEMPERATURE = 0.0f;
static constexpr float MAX_ROOM_TEMPERATURE = 40.0f;
// Room setpoints the display accepts, °C.
static constexpr uint16_t MIN_ROOM_SETPOINT = 10;
static constexpr uint16_t MAX_ROOM_SETPOINT = 30;
#endif

static uint16_t get_u16(const uint8_t *data) { return static_cast<uint16_t>((data[0] << 8) | data[1]); }

static void put_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value & 0xFF);
}

static bool crc_ok(const uint8_t *frame, size_t len) {
  return crc16(frame, len - 2) == static_cast<uint16_t>(frame[len - 2] | (frame[len - 1] << 8));
}

bool RegisterBlock::get(uint16_t address, uint16_t &value) const {
  if (address < this->start || address - this->start >= this->count)
    return false;
  value = get_u16(this->data + 2 * (address - this->start));
  return true;
}

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
#ifdef USE_NUMBER
void RoomSetpointNumber::control(float value) { this->parent_->request_room_setpoint(value); }
#endif

#ifdef USE_BUTTON
void HotWaterTopUpButton::press_action() { this->parent_->request_hot_water_top_up(); }
#endif

#ifdef USE_SWITCH
void RoomSensorSwitch::write_state(bool state) {
  this->parent_->set_room_sensor_enabled(state);
  this->publish_state(state);
}
#endif
#endif

void ThermiaItecEco::setup() {
  this->last_frame_ms_ = millis();
  if (this->bus_timeout_ms_ > 0)
    this->set_interval(1000, [this]() { this->check_bus_timeout_(); });

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  this->room_temperature_input_->add_on_state_callback([this](float value) {
    if (!std::isnan(value)) {
      this->room_input_ms_ = millis();
      this->room_input_seen_ = true;
    }
  });
#ifdef USE_SWITCH
  if (this->room_sensor_switch_ != nullptr) {
    bool enabled = this->room_sensor_switch_->get_initial_state_with_restore_mode().value_or(false);
    this->set_room_sensor_enabled(enabled);
    this->room_sensor_switch_->publish_state(enabled);
  }
#endif
#endif
}

void ThermiaItecEco::loop() {
  this->read_bus_();
  this->parse_buffer_();

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  if (this->reply_due_) {
    this->reply_due_ = false;
    // Anything after the poll means the bus has moved on (or another sensor is answering).
    if (this->rx_len_ == 0 && this->available() == 0)
      this->send_room_sensor_reply_();
  }
#endif
}

void ThermiaItecEco::read_bus_() {
  const uint32_t now = App.get_loop_component_start_time();
  size_t available = this->available();
  if (available == 0) {
    // A partial frame that stopped growing is noise: skip one byte and look for the next frame.
    if (this->rx_len_ > 0 && now - this->last_rx_ms_ > RX_STALE_MS) {
      std::memmove(this->rx_buffer_.data(), this->rx_buffer_.data() + 1, this->rx_len_ - 1);
      this->rx_len_--;
      this->last_rx_ms_ = now;
    }
    return;
  }
  while (available > 0) {
    if (this->rx_len_ == this->rx_buffer_.size()) {
      // Full without a frame: drop the older half (parse_buffer_() keeps at most one partial frame).
      std::memmove(this->rx_buffer_.data(), this->rx_buffer_.data() + MAX_FRAME, this->rx_len_ - MAX_FRAME);
      this->rx_len_ -= MAX_FRAME;
    }
    size_t chunk = std::min(available, this->rx_buffer_.size() - this->rx_len_);
    if (!this->read_array(this->rx_buffer_.data() + this->rx_len_, chunk))
      break;
    this->rx_len_ += chunk;
    available -= chunk;
    this->parse_buffer_();
  }
  this->last_rx_ms_ = now;
  this->last_rx_us_ = micros();
}

size_t ThermiaItecEco::match_frame(const uint8_t *data, size_t available, bool &incomplete) {
  incomplete = false;
  if (available < 2)
    return 0;
  const uint8_t function = data[1];

  // The frame length follows from the function code; requests and replies differ, so try both.
  std::array<size_t, 2> candidates{};
  size_t count = 0;
  bool need_more = false;
  switch (function) {
    case FUNCTION_READ_HOLDING:
    case FUNCTION_READ_INPUT:
      candidates[count++] = 8;  // request
      if (available > 2) {
        candidates[count++] = 5 + data[2];  // reply
      } else {
        need_more = true;
      }
      break;
    case FUNCTION_WRITE_MULTIPLE:
      candidates[count++] = 8;  // reply
      if (available > 6) {
        candidates[count++] = 9 + data[6];  // request
      } else {
        need_more = true;
      }
      break;
    case FUNCTION_READ_WRITE_MULTIPLE:
      if (available > 2) {
        candidates[count++] = 5 + data[2];  // reply
      } else {
        need_more = true;
      }
      if (available > 10) {
        candidates[count++] = 13 + data[10];  // request
      } else {
        need_more = true;
      }
      break;
    default:
      if ((function & FUNCTION_EXCEPTION) == 0)
        return 0;
      candidates[count++] = 5;
      break;
  }
  if (count == 2 && candidates[1] < candidates[0])
    std::swap(candidates[0], candidates[1]);

  for (size_t i = 0; i < count; i++) {
    const size_t len = candidates[i];
    if (len > MAX_FRAME)
      continue;
    if (len > available) {
      need_more = true;
      continue;
    }
    if (crc_ok(data, len))
      return len;
  }
  incomplete = need_more;
  return 0;
}

void ThermiaItecEco::parse_buffer_() {
  size_t pos = 0;
  while (pos < this->rx_len_) {
    bool incomplete;
    const size_t len = match_frame(this->rx_buffer_.data() + pos, this->rx_len_ - pos, incomplete);
    if (len > 0) {
      this->handle_frame_(this->rx_buffer_.data() + pos, len);
      pos += len;
    } else if (incomplete) {
      break;
    } else {
      pos++;
    }
  }
  if (pos > 0) {
    std::memmove(this->rx_buffer_.data(), this->rx_buffer_.data() + pos, this->rx_len_ - pos);
    this->rx_len_ -= pos;
  }
}

void ThermiaItecEco::handle_frame_(const uint8_t *frame, size_t len) {
  this->last_frame_ms_ = App.get_loop_component_start_time();
  if (this->bus_lost_) {
    this->bus_lost_ = false;
    ESP_LOGI(TAG, "Receiving heat pump data again");
    bool clear_warning = true;
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
    // Keep the warning for another room sensor on the bus.
    clear_warning = !this->peer_detected_;
#endif
    if (clear_warning)
      this->status_clear_warning();
  }

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  if (this->echo_expected_) {
    this->echo_expected_ = false;
    // Our own reply, echoed by the transceiver.
    if (len == this->last_reply_.size() && std::memcmp(frame, this->last_reply_.data(), len) == 0)
      return;
  }
#endif

  const uint8_t slave = frame[0];
  const uint8_t function = frame[1];
  ESP_LOGVV(TAG, "Frame from/to 0x%02X, function 0x%02X, %u bytes", slave, function, static_cast<unsigned>(len));

  if ((function & FUNCTION_EXCEPTION) != 0) {
    this->request_pending_ = false;
    return;
  }

  // A slave's reply to the display's last request. Replies carry no register address.
  const bool reads = function == FUNCTION_READ_HOLDING || function == FUNCTION_READ_INPUT ||
                     function == FUNCTION_READ_WRITE_MULTIPLE;
  if (reads && this->request_pending_ && this->request_.slave == slave && this->request_.function == function &&
      len == 5 + 2 * static_cast<size_t>(this->request_.count) && frame[2] == 2 * this->request_.count) {
    this->request_pending_ = false;
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
    if (slave == SLAVE_ROOM_SENSOR && function == FUNCTION_READ_WRITE_MULTIPLE)
      this->on_room_sensor_peer_();
#endif
    this->handle_read_(slave, RegisterBlock{this->request_.start, this->request_.count, frame + 3});
    return;
  }

  switch (function) {
    case FUNCTION_READ_HOLDING:
    case FUNCTION_READ_INPUT:
      if (len == 8) {
        this->request_ = Request{slave, function, get_u16(frame + 2), get_u16(frame + 4)};
        this->request_pending_ = true;
      }
      return;
    case FUNCTION_WRITE_MULTIPLE: {
      this->request_pending_ = false;
      const uint16_t count = get_u16(frame + 4);
      if (len == 9 + static_cast<size_t>(frame[6]) && frame[6] == 2 * count)
        this->handle_write_(slave, RegisterBlock{get_u16(frame + 2), count, frame + 7});
      return;
    }
    case FUNCTION_READ_WRITE_MULTIPLE: {
      const uint16_t write_count = get_u16(frame + 8);
      if (len < 13 || len != 13 + static_cast<size_t>(frame[10]) || frame[10] != 2 * write_count)
        return;
      const uint16_t read_start = get_u16(frame + 2);
      const uint16_t read_count = get_u16(frame + 4);
      this->request_ = Request{slave, function, read_start, read_count};
      this->request_pending_ = true;
      this->handle_write_(slave, RegisterBlock{get_u16(frame + 6), write_count, frame + 11});
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
      if (slave == SLAVE_ROOM_SENSOR && read_start == REG_ROOM_TEMPERATURE && read_count == ROOM_SENSOR_READ_COUNT)
        this->on_room_sensor_poll_();
#endif
      return;
    }
    default:
      return;
  }
}

void ThermiaItecEco::handle_read_(uint8_t slave, const RegisterBlock &block) {
  uint16_t value;
#ifdef USE_SENSOR
  // Temperatures in whole °C (controller) or tenths (outdoor unit, room sensor), signed.
  auto whole = [&](uint16_t address, SensorType type) {
    if (block.get(address, value)) {
      const auto temperature = static_cast<int16_t>(value);
      this->publish_sensor_(type, temperature == NOT_CONNECTED ? NAN : static_cast<float>(temperature));
    }
  };
  auto tenths = [&](uint16_t address, SensorType type) {
    if (block.get(address, value))
      this->publish_sensor_(type, static_cast<int16_t>(value) / 10.0f);
  };
  // Unsigned values, NOT_AVAILABLE when the outdoor unit has none (high pressure while idle).
  auto scaled = [&](uint16_t address, SensorType type, float scale) {
    if (block.get(address, value))
      this->publish_sensor_(type, value == NOT_AVAILABLE ? NAN : value * scale);
  };
#endif

  switch (slave) {
    case SLAVE_CONTROLLER:
#ifdef USE_SENSOR
      whole(REG_SUPPLY_TEMPERATURE, SENSOR_TYPE_SUPPLY_TEMPERATURE);
      whole(REG_HOT_WATER_TOP_TEMPERATURE, SENSOR_TYPE_HOT_WATER_TOP_TEMPERATURE);
      whole(REG_HOT_WATER_TEMPERATURE, SENSOR_TYPE_HOT_WATER_TEMPERATURE);
      whole(REG_OUTDOOR_TEMPERATURE, SENSOR_TYPE_OUTDOOR_TEMPERATURE);
#endif
      break;

    case SLAVE_OUTDOOR_UNIT:
#ifdef USE_SENSOR
      tenths(REG_CONDENSER_INLET, SENSOR_TYPE_CONDENSER_INLET_TEMPERATURE);
      tenths(REG_CONDENSER_OUTLET, SENSOR_TYPE_CONDENSER_OUTLET_TEMPERATURE);
      scaled(REG_HIGH_PRESSURE, SENSOR_TYPE_HIGH_PRESSURE, 0.1f);
      scaled(REG_COMPRESSOR_FREQUENCY, SENSOR_TYPE_COMPRESSOR_FREQUENCY, 1.0f);
      scaled(REG_COMPRESSOR_CURRENT, SENSOR_TYPE_COMPRESSOR_CURRENT, 0.1f);
      scaled(REG_OUTDOOR_FAN_SPEED, SENSOR_TYPE_OUTDOOR_FAN_SPEED, 1.0f);
      scaled(REG_EXPANSION_VALVE_POSITION, SENSOR_TYPE_EXPANSION_VALVE_POSITION, 1.0f);
#endif
#ifdef USE_BINARY_SENSOR
      if (block.get(REG_UNIT_STATE, value)) {
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_COMPRESSOR, (value & UNIT_STATE_COMPRESSOR) != 0);
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_OUTDOOR_FAN, (value & UNIT_STATE_OUTDOOR_FAN) != 0);
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_CIRCULATION_PUMP, (value & UNIT_STATE_CIRCULATION_PUMP) != 0);
      }
      if (block.get(REG_UNIT_STATUS, value))
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_DEFROST, (value & UNIT_STATUS_DEFROST) != 0);
#endif
      break;

    case SLAVE_ROOM_SENSOR:
#ifdef USE_SENSOR
      tenths(REG_ROOM_TEMPERATURE, SENSOR_TYPE_ROOM_TEMPERATURE);
#endif
      break;

    default:
      break;
  }
}

void ThermiaItecEco::handle_write_(uint8_t slave, const RegisterBlock &block) {
  uint16_t value;
#ifdef USE_SENSOR
  auto setting = [&](uint16_t address, SensorType type) {
    if (block.get(address, value))
      this->publish_sensor_(type, static_cast<int16_t>(value));
  };
#endif

  switch (slave) {
    case SLAVE_CONTROLLER:
#ifdef USE_BINARY_SENSOR
      if (block.get(REG_OUTPUTS, value)) {
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_HOT_WATER_VALVE, (value & OUTPUT_HOT_WATER_VALVE) != 0);
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_EXTERNAL_AUX_HEATER, (value & OUTPUT_EXTERNAL_AUX_HEATER) != 0);
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_IMMERSION_HEATER_1, (value & OUTPUT_IMMERSION_HEATER_1) != 0);
      }
      if (block.get(REG_OUTPUTS_2, value))
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_IMMERSION_HEATER_2, (value & OUTPUT_2_IMMERSION_HEATER_2) != 0);
#endif
#ifdef USE_SENSOR
      if (block.get(REG_CIRCULATION_PUMP_SPEED, value))
        this->publish_sensor_(SENSOR_TYPE_CIRCULATION_PUMP_SPEED, value);
#endif
      break;

    case SLAVE_OUTDOOR_UNIT:
#ifdef USE_SENSOR
      if (block.get(REG_TARGET_SUPPLY_TEMPERATURE, value))
        this->publish_sensor_(SENSOR_TYPE_TARGET_SUPPLY_TEMPERATURE, static_cast<int16_t>(value) / 10.0f);
#endif
      if (block.get(REG_MODE_REQUEST, value))
        this->publish_operating_mode_(value);
      break;

    case SLAVE_ROOM_SENSOR:
      if (block.get(REG_ROOM_SETPOINT, value))
        this->publish_room_setpoint_(value);
#ifdef USE_BINARY_SENSOR
      if (block.get(REG_ROOM_TOP_UP_STATUS, value))
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_HOT_WATER_TOP_UP, value == ROOM_TOP_UP_ACTIVE);
#endif
      break;

    case SLAVE_SETTINGS:
#ifdef USE_SENSOR
      setting(REG_HEATING_CURVE, SENSOR_TYPE_HEATING_CURVE);
      setting(REG_HEATING_CURVE_MIN, SENSOR_TYPE_HEATING_CURVE_MIN);
      setting(REG_HEATING_CURVE_MAX, SENSOR_TYPE_HEATING_CURVE_MAX);
      setting(REG_CURVE_CORRECTION_PLUS_5, SENSOR_TYPE_CURVE_CORRECTION_PLUS_5);
      setting(REG_CURVE_CORRECTION_0, SENSOR_TYPE_CURVE_CORRECTION_0);
      setting(REG_CURVE_CORRECTION_MINUS_5, SENSOR_TYPE_CURVE_CORRECTION_MINUS_5);
      setting(REG_HEATING_STOP_TEMPERATURE, SENSOR_TYPE_HEATING_STOP_TEMPERATURE);
      setting(REG_SETBACK_TEMPERATURE, SENSOR_TYPE_SETBACK_TEMPERATURE);
      setting(REG_ROOM_FACTOR, SENSOR_TYPE_ROOM_FACTOR);
#endif
#ifdef USE_BINARY_SENSOR
      if (block.get(REG_HIGH_POWER, value))
        this->publish_binary_sensor_(BINARY_SENSOR_TYPE_HIGH_POWER, value != 0);
#endif
      break;

    default:
      break;
  }
}

void ThermiaItecEco::check_bus_timeout_() {
  if (this->bus_lost_ || millis() - this->last_frame_ms_ <= this->bus_timeout_ms_)
    return;
  this->bus_lost_ = true;
  this->request_pending_ = false;
  ESP_LOGW(TAG, "No valid frame from the heat pump for %" PRIu32 " ms", this->bus_timeout_ms_);
  this->status_set_warning(LOG_STR("No data from heat pump"));
  this->invalidate_entities_();
}

void ThermiaItecEco::invalidate_entities_() {
#ifdef USE_SENSOR
  for (size_t i = 0; i < SENSOR_TYPE_COUNT; i++)
    this->publish_sensor_(static_cast<SensorType>(i), NAN);
#endif
#ifdef USE_BINARY_SENSOR
  for (auto *sens : this->binary_sensors_) {
    if (sens != nullptr)
      sens->invalidate_state();
  }
#endif
}

#ifdef USE_SENSOR
void ThermiaItecEco::publish_sensor_(SensorType type, float value) {
  sensor::Sensor *sens = this->sensors_[type];
  if (sens == nullptr)
    return;
  Deduplicator<float> &dedup = this->sensor_dedup_[type];
  if (std::isnan(value) ? dedup.next_unknown() : dedup.next(value))
    sens->publish_state(value);
}
#endif

#ifdef USE_BINARY_SENSOR
void ThermiaItecEco::publish_binary_sensor_(BinarySensorType type, bool state) {
  if (this->binary_sensors_[type] != nullptr)
    this->binary_sensors_[type]->publish_state(state);
}
#endif

void ThermiaItecEco::publish_operating_mode_(uint16_t mode) {
#ifdef USE_TEXT_SENSOR
  if (this->operating_mode_text_sensor_ == nullptr)
    return;
  const char *name;
  switch (mode) {
    case MODE_HEATING:
      name = "Heating";
      break;
    case MODE_HOT_WATER:
      name = "Hot water";
      break;
    case MODE_COOLING:
      name = "Cooling";
      break;
    default:
      name = "Unknown";
      break;
  }
  if (!this->operating_mode_text_sensor_->has_state() || this->operating_mode_text_sensor_->state != name)
    this->operating_mode_text_sensor_->publish_state(name);
#endif
}

void ThermiaItecEco::publish_room_setpoint_(uint16_t setpoint) {
#ifdef USE_SENSOR
  this->publish_sensor_(SENSOR_TYPE_ROOM_SETPOINT, setpoint);
#endif
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  this->controller_setpoint_ = setpoint;
  if (this->pending_setpoint_ != 0 && setpoint == this->pending_setpoint_) {
    ESP_LOGI(TAG, "Controller adopted room setpoint %u °C", setpoint);
    this->pending_setpoint_ = 0;
  }
#ifdef USE_NUMBER
  // While a request is pending the number keeps showing the requested value.
  if (this->room_setpoint_number_ != nullptr && this->pending_setpoint_ == 0 &&
      (!this->room_setpoint_number_->has_state() || this->room_setpoint_number_->state != setpoint))
    this->room_setpoint_number_->publish_state(setpoint);
#endif
#endif
}

void ThermiaItecEco::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Thermia iTec Eco:\n"
                "  Bus timeout: %" PRIu32 " ms",
                this->bus_timeout_ms_);
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  ESP_LOGCONFIG(TAG,
                "  Room sensor emulation: %s\n"
                "    Input timeout: %" PRIu32 " ms",
                this->room_sensor_enabled_ ? "enabled" : "disabled", this->room_input_timeout_ms_);
#endif
#ifdef USE_SENSOR
  for (auto *sens : this->sensors_) {
    if (sens != nullptr)
      LOG_SENSOR("  ", "Sensor", sens);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto *sens : this->binary_sensors_) {
    if (sens != nullptr)
      LOG_BINARY_SENSOR("  ", "Binary sensor", sens);
  }
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Operating mode", this->operating_mode_text_sensor_);
#endif
}

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
void ThermiaItecEco::set_room_sensor_enabled(bool enabled) {
  if (enabled == this->room_sensor_enabled_)
    return;
  this->room_sensor_enabled_ = enabled;
  if (!enabled) {
    this->pending_setpoint_ = 0;
    this->pending_top_up_ = false;
    this->reply_due_ = false;
  }
  ESP_LOGI(TAG, "Room sensor emulation %s", enabled ? "enabled" : "disabled");
}

bool ThermiaItecEco::room_input_fresh_() const {
  if (!this->room_input_seen_)
    return false;
  const float temperature = this->room_temperature_input_->state;
  if (std::isnan(temperature) || temperature < MIN_ROOM_TEMPERATURE || temperature > MAX_ROOM_TEMPERATURE)
    return false;
  return this->room_input_timeout_ms_ == 0 || millis() - this->room_input_ms_ <= this->room_input_timeout_ms_;
}

void ThermiaItecEco::request_room_setpoint(float setpoint) {
  const auto value = static_cast<uint16_t>(std::lround(clamp<float>(setpoint, MIN_ROOM_SETPOINT, MAX_ROOM_SETPOINT)));
  if (!this->room_sensor_enabled_ || this->peer_detected_) {
    ESP_LOGW(TAG, "Room setpoint %u °C not requested: room sensor emulation is off", value);
#ifdef USE_NUMBER
    // Show the controller's setpoint again instead of the rejected value.
    if (this->room_setpoint_number_ != nullptr && this->controller_setpoint_ != 0)
      this->room_setpoint_number_->publish_state(this->controller_setpoint_);
#endif
    return;
  }
  this->setpoint_replies_ = 0;
  this->pending_setpoint_ = value == this->controller_setpoint_ ? 0 : value;
  ESP_LOGD(TAG, "Requesting room setpoint %u °C", value);
#ifdef USE_NUMBER
  if (this->room_setpoint_number_ != nullptr)
    this->room_setpoint_number_->publish_state(value);
#endif
}

void ThermiaItecEco::request_hot_water_top_up() {
  if (!this->room_sensor_enabled_ || this->peer_detected_) {
    ESP_LOGW(TAG, "Hot water top-up not requested: room sensor emulation is off");
    return;
  }
  ESP_LOGD(TAG, "Requesting hot water top-up");
  this->pending_top_up_ = true;
}

void ThermiaItecEco::on_room_sensor_poll_() {
  if (!this->room_sensor_enabled_ || this->peer_detected_)
    return;
  if (!this->room_input_fresh_()) {
    ESP_LOGV(TAG, "No fresh room temperature: not answering the room sensor poll");
    return;
  }
  this->reply_due_ = true;
}

void ThermiaItecEco::send_room_sensor_reply_() {
  if (this->pending_setpoint_ != 0 && this->setpoint_replies_++ >= MAX_SETPOINT_REPLIES) {
    ESP_LOGW(TAG, "Controller did not adopt room setpoint %u °C", this->pending_setpoint_);
    this->pending_setpoint_ = 0;
#ifdef USE_NUMBER
    if (this->room_setpoint_number_ != nullptr && this->controller_setpoint_ != 0)
      this->room_setpoint_number_->publish_state(this->controller_setpoint_);
#endif
  }
  const float temperature = this->room_temperature_input_->state;
  const auto raw_temperature = static_cast<int16_t>(std::lround(temperature * 10.0f));

  // FC23 reply: three registers, room temperature x10, setpoint request, top-up request.
  std::array<uint8_t, 11> &reply = this->last_reply_;
  reply[0] = SLAVE_ROOM_SENSOR;
  reply[1] = FUNCTION_READ_WRITE_MULTIPLE;
  reply[2] = 2 * ROOM_SENSOR_READ_COUNT;
  put_u16(&reply[3], static_cast<uint16_t>(raw_temperature));
  put_u16(&reply[5], this->pending_setpoint_);
  put_u16(&reply[7], this->pending_top_up_ ? 1 : 0);
  const uint16_t crc = crc16(reply.data(), reply.size() - 2);
  reply[9] = static_cast<uint8_t>(crc & 0xFF);
  reply[10] = static_cast<uint8_t>(crc >> 8);
  if (this->pending_top_up_)
    ESP_LOGI(TAG, "Sending hot water top-up request");
  this->pending_top_up_ = false;

  // Keep the Modbus frame gap after the display's poll.
  const uint32_t since_rx = micros() - this->last_rx_us_;
  if (since_rx < FRAME_GAP_US)
    delayMicroseconds(FRAME_GAP_US - since_rx);
  this->write_array(reply.data(), reply.size());
  this->echo_expected_ = true;
  ESP_LOGV(TAG, "Room sensor reply: %.1f °C, setpoint request %u", temperature, this->pending_setpoint_);
#ifdef USE_SENSOR
  this->publish_sensor_(SENSOR_TYPE_ROOM_TEMPERATURE, raw_temperature / 10.0f);
#endif
}

void ThermiaItecEco::on_room_sensor_peer_() {
  if (this->peer_detected_)
    return;
  this->peer_detected_ = true;
  this->reply_due_ = false;
  this->pending_setpoint_ = 0;
  this->pending_top_up_ = false;
  ESP_LOGW(TAG, "Another room sensor answers on the bus: room sensor emulation stays off");
  this->status_set_warning(LOG_STR("Another room sensor on the bus"));
}
#endif

}  // namespace esphome::thermia_itec_eco
