#pragma once

#include <cmath>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/modbus_controller/modbus_controller.h"
#include "esphome/components/number/number.h"

namespace esphome::sunspec {

static constexpr uint16_t BASE_ADDR       = 40000;
static constexpr uint16_t REG_COUNT       = 178;  // 40000-40177, incl. end model header
static constexpr uint8_t  MAX_CLIENTS     = 8;    // compile-time upper bound; runtime limit is max_connections
static constexpr uint16_t MAX_BUF         = 260;
static constexpr uint32_t IDLE_TIMEOUT_MS = 300000;  // reap clients silent for 5 minutes

// SunSpec sentinel values for "not implemented"
static constexpr uint16_t NI_INT16  = 0x8000;  // int16 / sunssf not implemented
static constexpr uint16_t NI_UINT16 = 0xFFFF;  // uint16 / enum16 not implemented

// SunSpec inverter operating states (St)
static constexpr uint16_t ST_OFF      = 1;
static constexpr uint16_t ST_SLEEPING = 2;
static constexpr uint16_t ST_MPPT     = 4;

// SunSpec register addresses
static constexpr uint16_t REG_WMAXLIMPCT  = 40155;
static constexpr uint16_t REG_WMAXLIM_ENA = 40159;
// Model 123 writable window (WinTms/RvrtTms/Ena flags etc.); scale factors at
// 40173-40175 are read-only
static constexpr uint16_t WRITABLE_FIRST = 40152;
static constexpr uint16_t WRITABLE_LAST  = 40172;

// Slots for tracking per-sensor freshness (see sensor_last_update_)
enum SensorSlot : uint8_t {
  SLOT_AC_POWER = 0,
  SLOT_AC_VOLTAGE,       // single phase: PhVphA; three phase: phase A voltage
  SLOT_AC_CURRENT,       // total current
  SLOT_AC_CURRENT_PHA,   // three phase only
  SLOT_AC_CURRENT_PHB,
  SLOT_AC_CURRENT_PHC,
  SLOT_AC_VOLTAGE_PHB,
  SLOT_AC_VOLTAGE_PHC,
  SLOT_AC_FREQUENCY,
  SLOT_TEMPERATURE,
  SLOT_ENERGY,
  SLOT_DC_POWER,
  SLOT_DC_VOLTAGE,
  SLOT_DC_CURRENT,
  SLOT_COUNT,
};

struct Client {
  int      fd{-1};
  uint8_t  buf[MAX_BUF];
  uint16_t buf_len{0};
  uint32_t last_recv_ms{0};
};

class SunspecComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Setters called from __init__.py generated code
  void set_port(uint16_t port)                { this->port_ = port; }
  void set_max_connections(uint8_t n)         { this->max_connections_ = n; }
  void set_unit_address(uint8_t addr)         { this->unit_address_ = addr; }
  void set_stale_timeout(uint32_t timeout_ms) { this->stale_timeout_ms_ = timeout_ms; }
  void set_model_id(uint16_t model_id)        { this->model_id_ = model_id; }
  void set_manufacturer(const std::string &s) { this->manufacturer_ = s; }
  void set_model(const std::string &s)        { this->model_ = s; }
  void set_serial_number(const std::string &s){ this->serial_number_ = s; }
  void set_version(const std::string &s)      { this->version_ = s; }
  void set_rated_power(uint16_t w)            { this->rated_power_ = w; }

  void set_ac_power(sensor::Sensor *s)     { this->sensors_[SLOT_AC_POWER] = s; }
  void set_ac_voltage(sensor::Sensor *s)   { this->sensors_[SLOT_AC_VOLTAGE] = s; }
  void set_ac_current(sensor::Sensor *s)   { this->sensors_[SLOT_AC_CURRENT] = s; }
  void set_ac_current_phase_a(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHA] = s; }
  void set_ac_current_phase_b(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHB] = s; }
  void set_ac_current_phase_c(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHC] = s; }
  void set_ac_voltage_phase_b(sensor::Sensor *s) { this->sensors_[SLOT_AC_VOLTAGE_PHB] = s; }
  void set_ac_voltage_phase_c(sensor::Sensor *s) { this->sensors_[SLOT_AC_VOLTAGE_PHC] = s; }
  void set_ac_frequency(sensor::Sensor *s) { this->sensors_[SLOT_AC_FREQUENCY] = s; }
  void set_temperature(sensor::Sensor *s)  { this->sensors_[SLOT_TEMPERATURE] = s; }
  void set_energy_total(sensor::Sensor *s) { this->sensors_[SLOT_ENERGY] = s; }
  void set_dc_power(sensor::Sensor *s)     { this->sensors_[SLOT_DC_POWER] = s; }
  void set_dc_voltage(sensor::Sensor *s)   { this->sensors_[SLOT_DC_VOLTAGE] = s; }
  void set_dc_current(sensor::Sensor *s)   { this->sensors_[SLOT_DC_CURRENT] = s; }

  void set_modbus_controller(modbus_controller::ModbusController *ctrl) { this->controller_ = ctrl; }
  void set_power_limit_register(uint16_t reg) { this->power_limit_register_ = reg; }
  void set_power_limit_number(number::Number *n) { this->power_limit_number_ = n; }

  void add_on_power_limit_callback(std::function<void(float, bool)> &&callback) {
    this->power_limit_callback_.add(std::move(callback));
  }
  /// Last commanded power limit in percent of nameplate power (WMaxLimPct, SF=-2)
  float get_power_limit_pct() { return this->get_reg(REG_WMAXLIMPCT) * 0.01f; }
  /// Whether the commanded power limit is enabled (WMaxLim_Ena)
  bool get_power_limit_enabled() { return this->get_reg(REG_WMAXLIM_ENA) == 1; }

 protected:
  // Config
  uint16_t    port_{502};
  uint8_t     max_connections_{4};
  uint8_t     unit_address_{1};
  uint32_t    stale_timeout_ms_{300000};
  uint16_t    model_id_{101};  // 101 = single phase, 103 = three phase
  std::string manufacturer_;
  std::string model_;
  std::string serial_number_;
  std::string version_;
  uint16_t    rated_power_{0};

  // Sensors indexed by SensorSlot (ac_power, ac_voltage, ac_frequency,
  // temperature required; the rest optional)
  sensor::Sensor *sensors_[SLOT_COUNT]{};
  uint32_t        sensor_last_update_[SLOT_COUNT]{};
  bool            slot_fresh_[SLOT_COUNT]{};
  uint32_t        last_stale_check_{0};

  // Modbus write-back
  modbus_controller::ModbusController *controller_{nullptr};
  uint16_t power_limit_register_{0};
  number::Number *power_limit_number_{nullptr};
  CallbackManager<void(float, bool)> power_limit_callback_;

  // Register bank
  uint16_t registers_[REG_COUNT]{};

  // TCP server
  int    server_fd_{-1};
  Client clients_[MAX_CLIENTS];

  // Internal helpers
  void     init_static_registers_();
  void     encode_string_(uint16_t *dest, const std::string &s, uint8_t reg_count);
  void     update_slot_(uint8_t slot);
  void     check_stale_slots_();
  void     accept_clients_();
  void     process_client_(Client &c);
  void     handle_frame_(Client &c, uint16_t frame_len);
  void     send_response_(Client &c, uint16_t txid, uint8_t uid, const uint8_t *pdu, uint16_t pdu_len);
  void     send_exception_(Client &c, uint16_t txid, uint8_t uid, uint8_t fc, uint8_t code);
  void     apply_power_limit_();
  void     close_client_(Client &c);
  // Sensor state, or NaN when unset / never updated / stale
  float    fresh_state_(uint8_t slot);
  // Whether the total current point has any source (sensor or P/V derivation)
  bool     has_current_source_() {
    return this->sensors_[SLOT_AC_CURRENT] != nullptr || this->model_id_ == 101;
  }

  // Register helpers
  inline uint16_t get_reg(uint16_t addr)             { return this->registers_[addr - BASE_ADDR]; }
  inline void     set_reg(uint16_t addr, uint16_t v) { this->registers_[addr - BASE_ADDR] = v; }
  // Convert float to int16/uint16 with given scale factor (e.g. sf=-2 → multiply
  // by 100); NaN maps to the type's "not implemented" sentinel
  static uint16_t to_sf(float val, int sf);
  static uint16_t to_u16_sf(float val, int sf);
};

class PowerLimitTrigger : public Trigger<float, bool> {
 public:
  explicit PowerLimitTrigger(SunspecComponent *parent) {
    parent->add_on_power_limit_callback(
        [this](float level, bool enabled) { this->trigger(level, enabled); });
  }
};

}  // namespace esphome::sunspec
