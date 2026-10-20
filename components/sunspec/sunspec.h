#pragma once

#include <cmath>
#include <memory>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_SUNSPEC_MODBUS_CONTROLLER
#include "esphome/components/modbus_controller/modbus_controller.h"
#include "esphome/core/version.h"
#endif
#ifdef USE_SUNSPEC_POWER_LIMIT_NUMBER
#include "esphome/components/number/number.h"
#endif

// ModbusCommandItem / queue_command() are deprecated from 2026.9.0 (removed in 2027.3.0);
// from then on the limit is written by our own ModbusClientDevice on the controller's bus
#if defined(USE_SUNSPEC_MODBUS_CONTROLLER) && ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 9, 0)
#define USE_SUNSPEC_MODBUS_CLIENT_DEVICE
#endif

namespace esphome::sunspec {

static constexpr uint16_t BASE_ADDR = 40000;
static constexpr uint16_t REG_COUNT = 178;  // 40000-40177, incl. end model header
static constexpr uint16_t MAX_BUF = 260;
static constexpr uint32_t IDLE_TIMEOUT_MS = 300000;  // reap clients silent for 5 minutes

// SunSpec sentinel values for "not implemented"
static constexpr uint16_t NI_INT16 = 0x8000;   // int16 / sunssf not implemented
static constexpr uint16_t NI_UINT16 = 0xFFFF;  // uint16 / enum16 not implemented

// SunSpec inverter operating states (St)
static constexpr uint16_t ST_OFF = 1;
static constexpr uint16_t ST_SLEEPING = 2;
static constexpr uint16_t ST_MPPT = 4;

// Model 123 (Immediate Controls) register addresses
static constexpr uint16_t REG_CONN = 40154;
static constexpr uint16_t REG_WMAXLIMPCT = 40155;
static constexpr uint16_t REG_WMAXLIMPCT_RVRTTMS = 40157;
static constexpr uint16_t REG_WMAXLIM_ENA = 40159;
static constexpr uint16_t REG_OUTPFSET_ENA = 40164;
static constexpr uint16_t REG_VARPCT_ENA = 40172;
static constexpr uint16_t WMAXLIMPCT_MAX = 10000;  // 100.00 % at SF=-2
// Writable window (WinTms/RvrtTms/Ena flags etc.); scale factors at 40173-40175 are read-only
static constexpr uint16_t WRITABLE_FIRST = 40152;
static constexpr uint16_t WRITABLE_LAST = 40172;

// Slots for tracking per-sensor freshness (see sensor_last_update_)
enum SensorSlot : uint8_t {
  SLOT_AC_POWER = 0,
  SLOT_AC_VOLTAGE,      // single phase: PhVphA; three phase: phase A voltage
  SLOT_AC_CURRENT,      // total current
  SLOT_AC_CURRENT_PHA,  // three phase only
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
  std::unique_ptr<socket::Socket> sock;
  uint8_t buf[MAX_BUF];
  uint16_t buf_len{0};
  uint32_t last_recv_ms{0};
};

class SunspecComponent : public Component {
 public:
  // String arguments must outlive the component (codegen passes string literals)
  SunspecComponent(const char *manufacturer, const char *model, uint32_t rated_power, int8_t power_sf,
                   sensor::Sensor *ac_power, sensor::Sensor *ac_voltage, sensor::Sensor *ac_frequency,
                   sensor::Sensor *temperature)
      : manufacturer_(manufacturer), model_(model), rated_power_(rated_power), power_sf_(power_sf) {
    this->sensors_[SLOT_AC_POWER] = ac_power;
    this->sensors_[SLOT_AC_VOLTAGE] = ac_voltage;
    this->sensors_[SLOT_AC_FREQUENCY] = ac_frequency;
    this->sensors_[SLOT_TEMPERATURE] = temperature;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_connections(uint8_t n) { this->max_connections_ = n; }
  void set_unit_address(uint8_t addr) { this->unit_address_ = addr; }
  void set_stale_timeout(uint32_t timeout_ms) { this->stale_timeout_ms_ = timeout_ms; }
  void set_model_id(uint16_t model_id) { this->model_id_ = model_id; }
  void set_serial_number(const char *s) { this->serial_number_ = s; }
  void set_version(const char *s) { this->version_ = s; }

  void set_ac_current(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT] = s; }
  void set_ac_current_phase_a(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHA] = s; }
  void set_ac_current_phase_b(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHB] = s; }
  void set_ac_current_phase_c(sensor::Sensor *s) { this->sensors_[SLOT_AC_CURRENT_PHC] = s; }
  void set_ac_voltage_phase_b(sensor::Sensor *s) { this->sensors_[SLOT_AC_VOLTAGE_PHB] = s; }
  void set_ac_voltage_phase_c(sensor::Sensor *s) { this->sensors_[SLOT_AC_VOLTAGE_PHC] = s; }
  void set_energy_total(sensor::Sensor *s) { this->sensors_[SLOT_ENERGY] = s; }
  void set_dc_power(sensor::Sensor *s) { this->sensors_[SLOT_DC_POWER] = s; }
  void set_dc_voltage(sensor::Sensor *s) { this->sensors_[SLOT_DC_VOLTAGE] = s; }
  void set_dc_current(sensor::Sensor *s) { this->sensors_[SLOT_DC_CURRENT] = s; }

#ifdef USE_SUNSPEC_MODBUS_CONTROLLER
  void set_modbus_controller(modbus_controller::ModbusController *ctrl, uint16_t reg) {
    this->controller_ = ctrl;
    this->power_limit_register_ = reg;
#ifdef USE_SUNSPEC_MODBUS_CLIENT_DEVICE
    this->limit_writer_.set_parent(ctrl->hub());
    this->limit_writer_.set_address(ctrl->device_address());
#endif
  }
#endif
#ifdef USE_SUNSPEC_POWER_LIMIT_NUMBER
  void set_power_limit_number(number::Number *n) { this->power_limit_number_ = n; }
#endif

  template<typename F> void add_on_power_limit_callback(F &&callback) {
    this->power_limit_callback_.add(std::forward<F>(callback));
  }
  /// Last commanded power limit in percent of nameplate power (WMaxLimPct, SF=-2)
  float get_power_limit_pct() const { return this->get_reg_(REG_WMAXLIMPCT) * 0.01f; }
  /// Whether the commanded power limit is enabled (WMaxLim_Ena)
  bool get_power_limit_enabled() const { return this->get_reg_(REG_WMAXLIM_ENA) == 1; }

 protected:
  // Config
  uint16_t port_{502};
  uint8_t max_connections_{4};
  uint8_t unit_address_{1};
  uint32_t stale_timeout_ms_{300000};
  uint16_t model_id_{101};  // 101 = single phase, 103 = three phase
  const char *manufacturer_;
  const char *model_;
  const char *serial_number_{""};
  const char *version_{"1.0"};
  uint32_t rated_power_;
  int8_t power_sf_;  // shared by W, DCW and WRtg so large inverters fit in 16-bit registers

  // Sensors indexed by SensorSlot (ac_power, ac_voltage, ac_frequency,
  // temperature required; the rest optional)
  sensor::Sensor *sensors_[SLOT_COUNT]{};
  uint32_t sensor_last_update_[SLOT_COUNT]{};
  bool slot_fresh_[SLOT_COUNT]{};

  // Power limit write-back
#ifdef USE_SUNSPEC_MODBUS_CONTROLLER
  modbus_controller::ModbusController *controller_{nullptr};
  uint16_t power_limit_register_{0};
#endif
#ifdef USE_SUNSPEC_MODBUS_CLIENT_DEVICE
  // Reports a failed limit write back to the component, so the next command re-sends it
  class LimitWriter : public modbus::ModbusClientDevice {
   public:
    explicit LimitWriter(SunspecComponent *parent) : component_(parent) {}

   protected:
    void on_error(std::span<const uint8_t> request_pdu, modbus::ExceptionCode exception_code) override;
    bool on_no_response(std::span<const uint8_t> request_pdu) override;
    void on_not_sent(std::span<const uint8_t> request_pdu) override;
    SunspecComponent *component_;
  };
  LimitWriter limit_writer_{this};
  void on_limit_write_failed_(const char *reason);
#endif
#ifdef USE_SUNSPEC_POWER_LIMIT_NUMBER
  number::Number *power_limit_number_{nullptr};
#endif
  CallbackManager<void(float, bool)> power_limit_callback_;
  // Last limit forwarded to the inverter: WMaxLimPct raw value, or one of the sentinels below
  static constexpr uint32_t LIMIT_DISABLED = 0x10000;  // above any valid WMaxLimPct
  static constexpr uint32_t LIMIT_UNKNOWN = 0x10001;   // nothing forwarded since boot
  uint32_t applied_limit_{LIMIT_UNKNOWN};
  uint32_t applied_at_ms_{0};
  // WMaxLimPct_RvrtTms: revert to WMaxLim_Ena = 0 when no new limit command arrives in time
  uint32_t revert_start_ms_{0};
  uint32_t revert_after_ms_{0};  // 0 = no revert pending

  // Register bank
  uint16_t registers_[REG_COUNT]{};

  // TCP server
  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<Client[]> clients_;

  // Internal helpers
  void init_static_registers_();
  void encode_string_(uint16_t *dest, const char *s, uint8_t reg_count);
  void update_slot_(uint8_t slot);
  void check_stale_slots_();
  void accept_clients_(uint32_t now);
  void process_client_(Client &c, uint32_t now);
  void process_frames_(Client &c);
  void handle_frame_(Client &c, uint16_t frame_len);
  void send_response_(Client &c, uint16_t txid, uint8_t uid, const uint8_t *pdu, uint16_t pdu_len);
  void send_exception_(Client &c, uint16_t txid, uint8_t uid, uint8_t fc, uint8_t code);
  void on_limit_command_();
  void apply_power_limit_();
  void close_client_(Client &c);
  uint8_t client_index_(const Client &c) const { return static_cast<uint8_t>(&c - this->clients_.get()); }
  // Sensor state, or NaN when unset / never updated / stale
  float fresh_state_(uint8_t slot);
  // Whether the total current point has any source (sensor or P/V derivation)
  bool has_current_source_() const { return this->sensors_[SLOT_AC_CURRENT] != nullptr || this->model_id_ == 101; }

  // Register helpers
  uint16_t get_reg_(uint16_t addr) const { return this->registers_[addr - BASE_ADDR]; }
  void set_reg_(uint16_t addr, uint16_t v) { this->registers_[addr - BASE_ADDR] = v; }
  // Convert float to int16/uint16 with given scale factor (e.g. sf=-2 → multiply
  // by 100); NaN maps to the type's "not implemented" sentinel
  static uint16_t to_sf(float val, int sf);
  static uint16_t to_u16_sf(float val, int sf);
};

}  // namespace esphome::sunspec
