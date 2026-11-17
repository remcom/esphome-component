#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#endif

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::thermia_itec_eco {

// Modbus RTU frames are at most 256 bytes; the buffer holds one frame plus the start of the next.
static constexpr size_t MAX_FRAME = 256;
static constexpr size_t RX_BUFFER_SIZE = 2 * MAX_FRAME;

// Numeric values decoded from the bus (index into the hub's sensor table).
enum SensorType : uint8_t {
  SENSOR_TYPE_SUPPLY_TEMPERATURE,
  SENSOR_TYPE_OUTDOOR_TEMPERATURE,
  SENSOR_TYPE_HOT_WATER_TOP_TEMPERATURE,
  SENSOR_TYPE_HOT_WATER_TEMPERATURE,
  SENSOR_TYPE_ROOM_TEMPERATURE,
  SENSOR_TYPE_ROOM_SETPOINT,
  SENSOR_TYPE_TARGET_SUPPLY_TEMPERATURE,
  SENSOR_TYPE_CONDENSER_INLET_TEMPERATURE,
  SENSOR_TYPE_CONDENSER_OUTLET_TEMPERATURE,
  SENSOR_TYPE_HIGH_PRESSURE,
  SENSOR_TYPE_COMPRESSOR_FREQUENCY,
  SENSOR_TYPE_COMPRESSOR_CURRENT,
  SENSOR_TYPE_OUTDOOR_FAN_SPEED,
  SENSOR_TYPE_EXPANSION_VALVE_POSITION,
  SENSOR_TYPE_CIRCULATION_PUMP_SPEED,
  SENSOR_TYPE_HEATING_CURVE,
  SENSOR_TYPE_HEATING_CURVE_MIN,
  SENSOR_TYPE_HEATING_CURVE_MAX,
  SENSOR_TYPE_CURVE_CORRECTION_PLUS_5,
  SENSOR_TYPE_CURVE_CORRECTION_0,
  SENSOR_TYPE_CURVE_CORRECTION_MINUS_5,
  SENSOR_TYPE_HEATING_STOP_TEMPERATURE,
  SENSOR_TYPE_SETBACK_TEMPERATURE,
  SENSOR_TYPE_ROOM_FACTOR,
  SENSOR_TYPE_COUNT,
};

// On/off states decoded from the bus (index into the hub's binary sensor table).
enum BinarySensorType : uint8_t {
  BINARY_SENSOR_TYPE_COMPRESSOR,
  BINARY_SENSOR_TYPE_DEFROST,
  BINARY_SENSOR_TYPE_OUTDOOR_FAN,
  BINARY_SENSOR_TYPE_CIRCULATION_PUMP,
  BINARY_SENSOR_TYPE_HOT_WATER_VALVE,
  BINARY_SENSOR_TYPE_IMMERSION_HEATER_1,
  BINARY_SENSOR_TYPE_IMMERSION_HEATER_2,
  BINARY_SENSOR_TYPE_EXTERNAL_AUX_HEATER,
  BINARY_SENSOR_TYPE_HOT_WATER_TOP_UP,
  BINARY_SENSOR_TYPE_HIGH_POWER,
  BINARY_SENSOR_TYPE_COUNT,
};

// A run of big-endian 16-bit registers taken from a frame.
struct RegisterBlock {
  uint16_t start;
  uint16_t count;
  const uint8_t *data;

  // Raw value of register `address`; false when the block does not hold it.
  bool get(uint16_t address, uint16_t &value) const;
};

class ThermiaItecEco;

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
#ifdef USE_NUMBER
// Room setpoint: shows the controller's setpoint and requests a new one through the emulated sensor.
class RoomSetpointNumber : public number::Number, public Parented<ThermiaItecEco> {
 protected:
  void control(float value) override;
};
#endif

#ifdef USE_BUTTON
// Starts a hot water top-up through the emulated room sensor.
class HotWaterTopUpButton : public button::Button, public Parented<ThermiaItecEco> {
 protected:
  void press_action() override;
};
#endif

#ifdef USE_SWITCH
// Enables or disables the room sensor emulation.
class RoomSensorSwitch : public switch_::Switch, public Parented<ThermiaItecEco> {
 protected:
  void write_state(bool state) override;
};
#endif
#endif

class ThermiaItecEco : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_bus_timeout(uint32_t timeout_ms) { this->bus_timeout_ms_ = timeout_ms; }

#ifdef USE_SENSOR
  void set_sensor(SensorType type, sensor::Sensor *sens) { this->sensors_[type] = sens; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_binary_sensor(BinarySensorType type, binary_sensor::BinarySensor *sens) {
    this->binary_sensors_[type] = sens;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_operating_mode_text_sensor(text_sensor::TextSensor *sens) { this->operating_mode_text_sensor_ = sens; }
#endif

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  void set_room_sensor(sensor::Sensor *temperature, uint32_t timeout_ms) {
    this->room_temperature_input_ = temperature;
    this->room_input_timeout_ms_ = timeout_ms;
  }
#ifdef USE_NUMBER
  void set_room_setpoint_number(RoomSetpointNumber *number) { this->room_setpoint_number_ = number; }
#endif
#ifdef USE_BUTTON
  void set_hot_water_top_up_button(HotWaterTopUpButton *button) { this->hot_water_top_up_button_ = button; }
#endif
#ifdef USE_SWITCH
  void set_room_sensor_switch(RoomSensorSwitch *sw) { this->room_sensor_switch_ = sw; }
#endif

  // Asks the controller to adopt `setpoint` (whole °C) through the next room sensor replies.
  void request_room_setpoint(float setpoint);
  // Starts a hot water top-up with the next room sensor reply.
  void request_hot_water_top_up();
  void set_room_sensor_enabled(bool enabled);
#endif

 protected:
  void read_bus_();
  void parse_buffer_();
  // Length of the CRC-valid frame at the start of `data`, or 0. Sets `incomplete` when a longer
  // frame may still be arriving.
  static size_t match_frame(const uint8_t *data, size_t available, bool &incomplete);
  void handle_frame_(const uint8_t *frame, size_t len);
  // Registers the display reads from a slave (taken from the slave's reply).
  void handle_read_(uint8_t slave, const RegisterBlock &block);
  // Registers the display writes to a slave (taken from the display's request).
  void handle_write_(uint8_t slave, const RegisterBlock &block);
  void check_bus_timeout_();
  void invalidate_entities_();

#ifdef USE_SENSOR
  void publish_sensor_(SensorType type, float value);
#endif
#ifdef USE_BINARY_SENSOR
  void publish_binary_sensor_(BinarySensorType type, bool state);
#endif
  void publish_operating_mode_(uint16_t mode);
  void publish_room_setpoint_(uint16_t setpoint);

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  bool room_input_fresh_() const;
  void on_room_sensor_poll_();
  void send_room_sensor_reply_();
  void on_room_sensor_peer_();
#endif

  std::array<uint8_t, RX_BUFFER_SIZE> rx_buffer_{};
  size_t rx_len_{0};
  uint32_t last_rx_ms_{0};
  // When bytes last arrived, for the frame gap before a reply.
  uint32_t last_rx_us_{0};

  // The display's last request, so its reply (which carries no address) can be decoded.
  struct Request {
    uint8_t slave;
    uint8_t function;
    uint16_t start;
    uint16_t count;
  };
  Request request_{};
  bool request_pending_{false};

  uint32_t bus_timeout_ms_{0};
  uint32_t last_frame_ms_{0};
  bool bus_lost_{false};

#ifdef USE_SENSOR
  std::array<sensor::Sensor *, SENSOR_TYPE_COUNT> sensors_{};
  std::array<Deduplicator<float>, SENSOR_TYPE_COUNT> sensor_dedup_{};
#endif
#ifdef USE_BINARY_SENSOR
  std::array<binary_sensor::BinarySensor *, BINARY_SENSOR_TYPE_COUNT> binary_sensors_{};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *operating_mode_text_sensor_{nullptr};
#endif

#ifdef USE_THERMIA_ITEC_ECO_ROOM_SENSOR
  sensor::Sensor *room_temperature_input_{nullptr};
  uint32_t room_input_timeout_ms_{0};
  uint32_t room_input_ms_{0};
  bool room_input_seen_{false};
#ifdef USE_NUMBER
  RoomSetpointNumber *room_setpoint_number_{nullptr};
#endif
#ifdef USE_BUTTON
  HotWaterTopUpButton *hot_water_top_up_button_{nullptr};
#endif
#ifdef USE_SWITCH
  RoomSensorSwitch *room_sensor_switch_{nullptr};
#endif

  bool room_sensor_enabled_{true};
  // Another room sensor answered the display: never transmit (until reboot).
  bool peer_detected_{false};
  // The controller's room setpoint as last pushed to the room sensor slot (0 = not seen yet).
  uint16_t controller_setpoint_{0};
  // Setpoint requested in the room sensor replies (0 = no request) and how often it was sent.
  uint16_t pending_setpoint_{0};
  uint8_t setpoint_replies_{0};
  bool pending_top_up_{false};
  bool reply_due_{false};
  // Our last reply, to recognise it when the transceiver echoes it back.
  std::array<uint8_t, 11> last_reply_{};
  bool echo_expected_{false};
#endif
};

}  // namespace esphome::thermia_itec_eco
