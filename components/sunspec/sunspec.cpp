#include "sunspec.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::sunspec {

static const char *const TAG = "sunspec";

// Re-forward an unchanged power limit this often, so a write the inverter missed is retried
static constexpr uint32_t LIMIT_REFRESH_MS = 300000;

// ---------- helpers ----------

// 10^exp for the scale-factor exponents used here (-2..2)
static float pow10_exp(int exp) {
  static constexpr float POW10[] = {0.01f, 0.1f, 1.0f, 10.0f, 100.0f};
  return POW10[exp + 2];
}

uint16_t SunspecComponent::to_sf(float val, int sf) {
  if (std::isnan(val))
    return NI_INT16;
  float scaled = val * pow10_exp(-sf);
  if (scaled > 32767.0f)
    return 32767;
  if (scaled < -32767.0f)  // -32768 (0x8000) is the "not implemented" sentinel
    return static_cast<uint16_t>(static_cast<int16_t>(-32767));
  return static_cast<uint16_t>(static_cast<int16_t>(std::lround(scaled)));
}

uint16_t SunspecComponent::to_u16_sf(float val, int sf) {
  if (std::isnan(val))
    return NI_UINT16;
  float scaled = val * pow10_exp(-sf);
  // Cap below the 0xFFFF "not implemented" sentinel
  if (scaled < 0.0f)
    return 0;
  if (scaled > 65534.0f)
    return 65534;
  return static_cast<uint16_t>(std::lround(scaled));
}

static uint16_t sunssf(int sf) { return static_cast<uint16_t>(static_cast<int16_t>(sf)); }

// Registers that make up a power limit command (value, enable, revert timeout)
static bool is_limit_register(uint16_t addr) {
  return addr == REG_WMAXLIMPCT || addr == REG_WMAXLIM_ENA || addr == REG_WMAXLIMPCT_RVRTTMS;
}

// Range check for writable Model 123 points; out-of-range values get exception 0x03
static bool is_valid_control_value(uint16_t addr, uint16_t val) {
  switch (addr) {
    case REG_WMAXLIMPCT:
      return val <= WMAXLIMPCT_MAX;
    case REG_CONN:
    case REG_WMAXLIM_ENA:
    case REG_OUTPFSET_ENA:
    case REG_VARPCT_ENA:
      return val <= 1;
    default:
      return true;
  }
}

float SunspecComponent::fresh_state_(uint8_t slot) {
  sensor::Sensor *s = this->sensors_[slot];
  if (s == nullptr || !s->has_state())
    return NAN;
  if (this->stale_timeout_ms_ > 0 && millis() - this->sensor_last_update_[slot] > this->stale_timeout_ms_)
    return NAN;
  return s->get_state();
}

void SunspecComponent::encode_string_(uint16_t *dest, const char *s, uint8_t reg_count) {
  // Pack ASCII bytes as big-endian uint16 pairs, null-padded
  size_t len = strnlen(s, reg_count * 2u);
  for (uint8_t i = 0; i < reg_count; i++) {
    uint8_t hi = (i * 2u < len) ? static_cast<uint8_t>(s[i * 2]) : 0;
    uint8_t lo = (i * 2u + 1 < len) ? static_cast<uint8_t>(s[i * 2 + 1]) : 0;
    dest[i] = static_cast<uint16_t>((hi << 8) | lo);
  }
}

// ---------- static register map ----------

void SunspecComponent::init_static_registers_() {
  // Initialise entire bank to 0xFFFF (SunSpec uint16 "not implemented")
  for (auto &reg : this->registers_)
    reg = NI_UINT16;

  // --- Common Block (Model 1) ---
  this->set_reg_(40000, 0x5375);  // SunS marker high
  this->set_reg_(40001, 0x6e53);  // SunS marker low
  this->set_reg_(40002, 1);       // Model ID
  this->set_reg_(40003, 66);      // Length (data registers only)

  this->encode_string_(&this->registers_[4], this->manufacturer_, 16);    // 40004-40019
  this->encode_string_(&this->registers_[20], this->model_, 16);          // 40020-40035
  this->encode_string_(&this->registers_[36], "", 8);                     // 40036-40043 (options, empty)
  this->encode_string_(&this->registers_[44], this->version_, 8);         // 40044-40051
  this->encode_string_(&this->registers_[52], this->serial_number_, 16);  // 40052-40067

  this->set_reg_(40068, this->unit_address_);  // DA (Modbus device address)
  this->set_reg_(40069, 0);                    // Padding

  // --- Inverter Block (Model 101 single phase / 103 three phase) ---
  this->set_reg_(40070, this->model_id_);  // Model ID
  this->set_reg_(40071, 50);               // Length
  // 40072-40075: A/AphA/AphB/AphC -- updated in update_slot_()
  bool has_current = this->has_current_source_() || this->sensors_[SLOT_AC_CURRENT_PHA] != nullptr ||
                     this->sensors_[SLOT_AC_CURRENT_PHB] != nullptr || this->sensors_[SLOT_AC_CURRENT_PHC] != nullptr;
  this->set_reg_(40076, has_current ? sunssf(-2) : NI_INT16);  // A_SF
  // 40077-40079: phase-to-phase voltage AB/BC/CA -- stay NI
  // 40080-40082: PhVphA/BN/CN -- updated in update_slot_() where configured
  this->set_reg_(40083, sunssf(-1));               // V_SF = -1
  this->set_reg_(40084, NI_INT16);                 // W -- updated in update_slot_()
  this->set_reg_(40085, sunssf(this->power_sf_));  // W_SF
  // 40086: Hz -- updated in update_slot_()
  this->set_reg_(40087, sunssf(-2));  // Hz_SF = -2
  // 40088-40093: VA/VAr/PF and their SFs -- int16 "not implemented"
  for (uint16_t addr = 40088; addr <= 40093; addr++)
    this->set_reg_(addr, NI_INT16);
  this->set_reg_(40094, 0);          // WH high -- updated in update_slot_()
  this->set_reg_(40095, 0);          // WH low
  this->set_reg_(40096, 0);          // WH_SF = 0
  this->set_reg_(40097, NI_UINT16);  // DCA -- updated in update_slot_() if configured
  this->set_reg_(40098, this->sensors_[SLOT_DC_CURRENT] != nullptr ? sunssf(-2) : NI_INT16);             // DCA_SF
  this->set_reg_(40099, NI_UINT16);                                                                      // DCV
  this->set_reg_(40100, this->sensors_[SLOT_DC_VOLTAGE] != nullptr ? sunssf(-1) : NI_INT16);             // DCV_SF
  this->set_reg_(40101, NI_INT16);                                                                       // DCW
  this->set_reg_(40102, this->sensors_[SLOT_DC_POWER] != nullptr ? sunssf(this->power_sf_) : NI_INT16);  // DCW_SF
  this->set_reg_(40103, NI_INT16);    // TmpCab -- updated in update_slot_()
  this->set_reg_(40104, NI_INT16);    // TmpSnk
  this->set_reg_(40105, NI_INT16);    // TmpTrns
  this->set_reg_(40106, NI_INT16);    // TmpOt
  this->set_reg_(40107, sunssf(-1));  // Tmp_SF = -1
  this->set_reg_(40108, ST_OFF);      // St (updated in update_slot_())
  // 40109: StVnd -- stays NI
  // 40110-40121: Evt1/Evt2/EvtVnd1-4 = 0 (no events)
  for (uint16_t addr = 40110; addr <= 40121; addr++)
    this->set_reg_(addr, 0);

  // --- Nameplate Block (Model 120) ---
  this->set_reg_(40122, 120);                                                                 // Model ID
  this->set_reg_(40123, 26);                                                                  // Length
  this->set_reg_(40124, 4);                                                                   // DER type = PV
  this->set_reg_(40125, to_u16_sf(static_cast<float>(this->rated_power_), this->power_sf_));  // WRtg
  this->set_reg_(40126, sunssf(this->power_sf_));                                             // WRtg_SF
  // 40127-40149: stay NI

  // --- Immediate Controls Block (Model 123) ---
  this->set_reg_(40150, 123);  // Model ID
  this->set_reg_(40151, 24);   // Length
  // 40152-40153: Conn_WinTms / Conn_RvrtTms -- stay NI
  this->set_reg_(REG_CONN, 1);                     // Conn = connected
  this->set_reg_(REG_WMAXLIMPCT, WMAXLIMPCT_MAX);  // WMaxLimPct default = 100.00 % (SF=-2)
  // 40156-40158: WMaxLimPct WinTms/RvrtTms/RmpTms -- stay NI (RvrtTms is honoured when written)
  this->set_reg_(REG_WMAXLIM_ENA, 0);  // WMaxLim_Ena = 0 (disabled)
  this->set_reg_(40160, NI_INT16);     // OutPFSet
  // 40161-40163: OutPFSet WinTms/RvrtTms/RmpTms -- stay NI
  this->set_reg_(REG_OUTPFSET_ENA, 0);  // OutPFSet_Ena = 0 (disabled)
  this->set_reg_(40165, NI_INT16);      // VArWMaxPct
  this->set_reg_(40166, NI_INT16);      // VArMaxPct
  this->set_reg_(40167, NI_INT16);      // VArAvalPct
  // 40168-40170: VArPct WinTms/RvrtTms/RmpTms -- stay NI
  // 40171: VArPct_Mod -- stays NI
  this->set_reg_(REG_VARPCT_ENA, 0);  // VArPct_Ena = 0 (disabled)
  this->set_reg_(40173, sunssf(-2));  // WMaxLimPct_SF = -2 (0.01 %)
  this->set_reg_(40174, NI_INT16);    // OutPFSet_SF
  this->set_reg_(40175, NI_INT16);    // VArPct_SF

  // --- End Model (ID 0xFFFF, length 0) ---
  this->set_reg_(40176, 0xFFFF);
  this->set_reg_(40177, 0);
}

// ---------- lifecycle ----------

void SunspecComponent::setup() {
  // Event-driven register updates: each sensor publish patches only its own
  // register slot(s). The timestamps let check_stale_slots_() report sensors
  // that stop updating as "not implemented" instead of serving frozen data.
  const uint32_t now = millis();
  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    sensor::Sensor *s = this->sensors_[slot];
    if (s == nullptr)
      continue;
    this->sensor_last_update_[slot] = now;
    s->add_on_state_callback([this, slot](float) {
      this->sensor_last_update_[slot] = millis();
      this->slot_fresh_[slot] = true;
      this->update_slot_(slot);
    });
  }

  // Initialise register bank, then seed live slots from sensors that
  // already have a state (e.g. restored values)
  this->init_static_registers_();
  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    if (this->sensors_[slot] == nullptr)
      continue;
    if (this->sensors_[slot]->has_state())
      this->slot_fresh_[slot] = true;
    this->update_slot_(slot);
  }

  this->clients_ = std::make_unique<Client[]>(this->max_connections_);

  // Non-blocking TCP server; loop-monitored so incoming data wakes the main loop
  this->server_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->server_ == nullptr) {
    this->mark_failed(LOG_STR("Could not create socket"));
    return;
  }

  int enable = 1;
  if (this->server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
    ESP_LOGW(TAG, "Setting SO_REUSEADDR failed: errno %d", errno);

  if (this->server_->setblocking(false) != 0) {
    this->server_ = nullptr;
    this->mark_failed(LOG_STR("Could not make socket non-blocking"));
    return;
  }

  struct sockaddr_storage addr {};
  socklen_t addr_len = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr), this->port_);
  if (addr_len == 0 || this->server_->bind(reinterpret_cast<struct sockaddr *>(&addr), addr_len) != 0) {
    this->server_ = nullptr;
    this->mark_failed(LOG_STR("Could not bind port (already in use?)"));
    return;
  }

  if (this->server_->listen(this->max_connections_) != 0) {
    this->server_ = nullptr;
    this->mark_failed(LOG_STR("Could not listen on socket"));
    return;
  }

  if (this->stale_timeout_ms_ > 0)
    this->set_interval(1000, [this]() { this->check_stale_slots_(); });

  ESP_LOGI(TAG, "SunSpec Modbus TCP server listening on port %u", this->port_);
}

void SunspecComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SunSpec Modbus TCP:\n"
                "  Manufacturer:    %s\n"
                "  Model:           %s\n"
                "  Serial:          %s\n"
                "  Version:         %s\n"
                "  Rated Power:     %" PRIu32 " W\n"
                "  Power SF:        %d\n"
                "  Inverter Model:  %u\n"
                "  Port:            %u\n"
                "  Max Connections: %u\n"
                "  Unit Address:    %u\n"
                "  Stale Timeout:   %" PRIu32 " ms",
                this->manufacturer_, this->model_, this->serial_number_, this->version_, this->rated_power_,
                this->power_sf_, this->model_id_, this->port_, this->max_connections_, this->unit_address_,
                this->stale_timeout_ms_);
  LOG_SENSOR("  ", "AC Power", this->sensors_[SLOT_AC_POWER]);
  LOG_SENSOR("  ", "AC Voltage", this->sensors_[SLOT_AC_VOLTAGE]);
  LOG_SENSOR("  ", "AC Current", this->sensors_[SLOT_AC_CURRENT]);
  LOG_SENSOR("  ", "AC Current Phase A", this->sensors_[SLOT_AC_CURRENT_PHA]);
  LOG_SENSOR("  ", "AC Current Phase B", this->sensors_[SLOT_AC_CURRENT_PHB]);
  LOG_SENSOR("  ", "AC Current Phase C", this->sensors_[SLOT_AC_CURRENT_PHC]);
  LOG_SENSOR("  ", "AC Voltage Phase B", this->sensors_[SLOT_AC_VOLTAGE_PHB]);
  LOG_SENSOR("  ", "AC Voltage Phase C", this->sensors_[SLOT_AC_VOLTAGE_PHC]);
  LOG_SENSOR("  ", "AC Frequency", this->sensors_[SLOT_AC_FREQUENCY]);
  LOG_SENSOR("  ", "Temperature", this->sensors_[SLOT_TEMPERATURE]);
  LOG_SENSOR("  ", "Energy Total", this->sensors_[SLOT_ENERGY]);
  LOG_SENSOR("  ", "DC Power", this->sensors_[SLOT_DC_POWER]);
  LOG_SENSOR("  ", "DC Voltage", this->sensors_[SLOT_DC_VOLTAGE]);
  LOG_SENSOR("  ", "DC Current", this->sensors_[SLOT_DC_CURRENT]);

  bool write_back = false;
#ifdef USE_SUNSPEC_POWER_LIMIT_NUMBER
  if (this->power_limit_number_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Power limit write-back: number '%s'", this->power_limit_number_->get_name().c_str());
    write_back = true;
  }
#endif
#ifdef USE_SUNSPEC_MODBUS_CONTROLLER
  if (this->controller_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Power limit write-back: Modbus register %u", this->power_limit_register_);
    write_back = true;
  }
#endif
  if (!write_back)
    ESP_LOGCONFIG(TAG, "  Power limit write-back: on_power_limit automations only");
}

void SunspecComponent::loop() {
  if (this->server_ == nullptr)
    return;

  const uint32_t now = App.get_loop_component_start_time();

  // WMaxLimPct_RvrtTms expired without a new limit command: fall back to full power
  if (this->revert_after_ms_ != 0 && now - this->revert_start_ms_ >= this->revert_after_ms_) {
    ESP_LOGI(TAG, "Power limit revert timeout (%" PRIu32 " s) expired, disabling limit", this->revert_after_ms_ / 1000);
    this->revert_after_ms_ = 0;
    this->set_reg_(REG_WMAXLIM_ENA, 0);
    this->apply_power_limit_();
  }

  if (this->server_->ready())
    this->accept_clients_(now);

  for (uint8_t i = 0; i < this->max_connections_; i++) {
    Client &c = this->clients_[i];
    if (c.sock != nullptr)
      this->process_client_(c, now);
  }
}

// Drains all pending connections, as the ready() contract requires
void SunspecComponent::accept_clients_(uint32_t now) {
  while (true) {
    struct sockaddr_storage addr {};
    socklen_t addr_len = sizeof(addr);
    auto sock = this->server_->accept_loop_monitored(reinterpret_cast<struct sockaddr *>(&addr), &addr_len);
    if (sock == nullptr)
      return;

    char peer[socket::SOCKADDR_STR_LEN];
    sock->getpeername_to(peer);

    // Find a free slot within the configured connection limit
    Client *slot = nullptr;
    for (uint8_t i = 0; i < this->max_connections_; i++) {
      if (this->clients_[i].sock == nullptr) {
        slot = &this->clients_[i];
        break;
      }
    }
    if (slot == nullptr) {
      ESP_LOGW(TAG, "Max clients (%u) reached, rejecting %s", this->max_connections_, peer);
      continue;  // dropping the socket closes the connection
    }
    if (sock->setblocking(false) != 0) {
      ESP_LOGW(TAG, "Could not make client %s non-blocking: errno %d", peer, errno);
      continue;
    }
    // Small responses to poll requests should not wait for Nagle
    int enable = 1;
    if (sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) != 0)
      ESP_LOGD(TAG, "Setting TCP_NODELAY for %s failed: errno %d", peer, errno);
    if (sock->setsockopt(SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable)) != 0)
      ESP_LOGD(TAG, "Setting SO_KEEPALIVE for %s failed: errno %d", peer, errno);

    slot->sock = std::move(sock);
    slot->buf_len = 0;
    slot->last_recv_ms = now;
    ESP_LOGD(TAG, "Client %u connected from %s", this->client_index_(*slot), peer);
  }
}

void SunspecComponent::close_client_(Client &c) {
  if (c.sock == nullptr)
    return;
  ESP_LOGD(TAG, "Closing client %u", this->client_index_(c));
  c.sock = nullptr;
  c.buf_len = 0;
}

// ---------- sensor updates (event-driven) ----------

void SunspecComponent::update_slot_(uint8_t slot) {
  switch (slot) {
    case SLOT_AC_POWER: {
      float pwr = this->fresh_state_(SLOT_AC_POWER);
      this->set_reg_(40084, to_sf(pwr, this->power_sf_));
      // Inverter state: no fresh power data -> Off; producing -> MPPT; else Sleeping
      uint16_t st = ST_OFF;
      if (!std::isnan(pwr))
        st = (pwr > 5.0f) ? ST_MPPT : ST_SLEEPING;
      this->set_reg_(40108, st);
      // Derived current depends on power
      if (this->sensors_[SLOT_AC_CURRENT] == nullptr && this->model_id_ == 101)
        this->update_slot_(SLOT_AC_CURRENT);
      break;
    }
    case SLOT_AC_VOLTAGE: {
      // AC voltage phase A (SF=-1): value x 10
      this->set_reg_(40080, to_u16_sf(this->fresh_state_(SLOT_AC_VOLTAGE), -1));
      // Derived current depends on voltage
      if (this->sensors_[SLOT_AC_CURRENT] == nullptr && this->model_id_ == 101)
        this->update_slot_(SLOT_AC_CURRENT);
      break;
    }
    case SLOT_AC_CURRENT: {
      // Total AC current (SF=-2). Single phase without a current sensor derives
      // it from power / voltage (assuming unity power factor).
      float curr = this->fresh_state_(SLOT_AC_CURRENT);
      if (this->sensors_[SLOT_AC_CURRENT] == nullptr && this->model_id_ == 101) {
        float pwr = this->fresh_state_(SLOT_AC_POWER);
        float volt = this->fresh_state_(SLOT_AC_VOLTAGE);
        if (!std::isnan(pwr) && !std::isnan(volt) && volt > 1.0f)
          curr = pwr / volt;
      }
      uint16_t raw = to_u16_sf(curr, -2);
      this->set_reg_(40072, raw);  // A (total)
      if (this->model_id_ == 101)
        this->set_reg_(40073, raw);  // AphA mirrors total
      break;
    }
    case SLOT_AC_CURRENT_PHA:
      this->set_reg_(40073, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_CURRENT_PHB:
      this->set_reg_(40074, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_CURRENT_PHC:
      this->set_reg_(40075, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_VOLTAGE_PHB:
      this->set_reg_(40081, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_AC_VOLTAGE_PHC:
      this->set_reg_(40082, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_AC_FREQUENCY:
      // AC frequency (SF=-2): value x 100
      this->set_reg_(40086, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_TEMPERATURE:
      // Temperature (SF=-1): value x 10
      this->set_reg_(40103, to_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_ENERGY: {
      // Energy (acc32, SF=0, sensor reports kWh). Accumulators keep their last
      // known total when the sensor goes stale, so bypass the staleness check.
      sensor::Sensor *energy = this->sensors_[SLOT_ENERGY];
      if (energy != nullptr && energy->has_state()) {
        float e = energy->get_state();
        if (!std::isnan(e) && e >= 0) {
          auto wh = static_cast<uint32_t>(std::min(static_cast<double>(e) * 1000.0, 4294967295.0));
          this->set_reg_(40094, static_cast<uint16_t>(wh >> 16));
          this->set_reg_(40095, static_cast<uint16_t>(wh & 0xFFFF));
        }
      }
      break;
    }
    case SLOT_DC_POWER:
      if (this->sensors_[slot] != nullptr)
        this->set_reg_(40101, to_sf(this->fresh_state_(slot), this->power_sf_));
      break;
    case SLOT_DC_VOLTAGE:
      if (this->sensors_[slot] != nullptr)
        this->set_reg_(40099, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_DC_CURRENT:
      if (this->sensors_[slot] != nullptr)
        this->set_reg_(40097, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    default:
      break;
  }
}

void SunspecComponent::check_stale_slots_() {
  const uint32_t now = millis();
  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    // Energy is an accumulator; the last known total stays valid
    if (this->sensors_[slot] == nullptr || slot == SLOT_ENERGY)
      continue;
    if (!this->slot_fresh_[slot])
      continue;
    if (now - this->sensor_last_update_[slot] < this->stale_timeout_ms_)
      continue;

    this->slot_fresh_[slot] = false;
    this->update_slot_(slot);  // fresh_state_() now returns NaN -> "not implemented"
    ESP_LOGW(TAG, "Sensor '%s' has not updated for %" PRIu32 " ms; reporting as not implemented",
             this->sensors_[slot]->get_name().c_str(), this->stale_timeout_ms_);
  }
}

// ---------- TCP framing ----------

void SunspecComponent::process_client_(Client &c, uint32_t now) {
  if (now - c.last_recv_ms > IDLE_TIMEOUT_MS) {
    ESP_LOGD(TAG, "Client %u idle timeout", this->client_index_(c));
    this->close_client_(c);
    return;
  }
  if (!c.sock->ready())
    return;

  // ready() contract: keep reading until the socket would block
  while (true) {
    ssize_t n = c.sock->read(c.buf + c.buf_len, MAX_BUF - c.buf_len);
    if (n == 0) {
      this->close_client_(c);
      return;
    }
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        this->close_client_(c);
      return;
    }
    c.buf_len += static_cast<uint16_t>(n);
    c.last_recv_ms = now;

    this->process_frames_(c);
    if (c.sock == nullptr)
      return;
  }
}

void SunspecComponent::process_frames_(Client &c) {
  while (c.buf_len >= 6) {
    uint16_t proto_id = (c.buf[2] << 8) | c.buf[3];
    uint16_t pdu_length = (c.buf[4] << 8) | c.buf[5];  // bytes after 6-byte MBAP header

    if (proto_id != 0x0000) {
      ESP_LOGD(TAG, "Invalid protocol ID %04X, closing", proto_id);
      this->close_client_(c);
      return;
    }

    // pdu_length must include at least unit-id + function-code bytes
    if (pdu_length < 2) {
      ESP_LOGD(TAG, "PDU length %u too short, closing", pdu_length);
      this->close_client_(c);
      return;
    }

    uint16_t frame_len = 6 + pdu_length;
    if (frame_len > MAX_BUF) {
      ESP_LOGD(TAG, "Frame too large (%u bytes), closing", frame_len);
      this->close_client_(c);
      return;
    }

    if (c.buf_len < frame_len)
      break;  // incomplete frame, wait for more data

    this->handle_frame_(c, frame_len);
    if (c.sock == nullptr)
      return;  // handler closed the connection

    c.buf_len -= frame_len;
    if (c.buf_len > 0)
      memmove(c.buf, c.buf + frame_len, c.buf_len);
  }

  // Buffer full with no complete frame extractable: the client is sending garbage
  if (c.buf_len >= MAX_BUF) {
    ESP_LOGD(TAG, "Client %u buffer overflow, closing", this->client_index_(c));
    this->close_client_(c);
  }
}

void SunspecComponent::handle_frame_(Client &c, uint16_t frame_len) {
  uint16_t txid = (c.buf[0] << 8) | c.buf[1];
  uint8_t uid = c.buf[6];
  uint8_t fc = c.buf[7];

  // Serve any unit ID: SunSpec clients probe at various unit IDs during
  // discovery (e.g. Victron scans IDs other than 1), and as a single-device
  // bridge there is nothing else a request could be routed to. The response
  // echoes the request's unit ID; the configured address is only reported in
  // the Model 1 DA register.
  if (uid != this->unit_address_ && uid != 0xFF) {
    ESP_LOGVV(TAG, "Serving request for unit %u (configured: %u)", uid, this->unit_address_);
  }

  if (fc == 0x03 || fc == 0x04) {
    // FC03/FC04: Read Holding / Input Registers (same register bank)
    if (frame_len < 12) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    uint16_t start = (c.buf[8] << 8) | c.buf[9];
    uint16_t count = (c.buf[10] << 8) | c.buf[11];

    if (count == 0 || count > 125) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    if (start < BASE_ADDR || static_cast<uint32_t>(start) + count > BASE_ADDR + REG_COUNT) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }

    // Max count is 125 registers → 2 + 250 = 252 bytes
    uint8_t pdu[2 + 125 * 2];
    pdu[0] = fc;
    pdu[1] = static_cast<uint8_t>(count * 2);
    uint16_t idx = start - BASE_ADDR;
    for (uint16_t i = 0; i < count; i++) {
      pdu[2 + i * 2] = this->registers_[idx + i] >> 8;
      pdu[2 + i * 2 + 1] = this->registers_[idx + i] & 0xFF;
    }
    this->send_response_(c, txid, uid, pdu, 2 + count * 2);

  } else if (fc == 0x06) {
    // FC06: Write Single Register
    if (frame_len < 12) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    uint16_t addr = (c.buf[8] << 8) | c.buf[9];
    uint16_t val = (c.buf[10] << 8) | c.buf[11];

    if (addr < WRITABLE_FIRST || addr > WRITABLE_LAST) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }
    if (!is_valid_control_value(addr, val)) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    this->set_reg_(addr, val);
    if (is_limit_register(addr))
      this->on_limit_command_();

    // Echo back per Modbus spec
    uint8_t pdu[5] = {fc, c.buf[8], c.buf[9], c.buf[10], c.buf[11]};
    this->send_response_(c, txid, uid, pdu, sizeof(pdu));

  } else if (fc == 0x10) {
    // FC16: Write Multiple Registers
    if (frame_len < 13) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    uint16_t start = (c.buf[8] << 8) | c.buf[9];
    uint16_t count = (c.buf[10] << 8) | c.buf[11];
    uint8_t byte_count = c.buf[12];

    if (count == 0 || count > 123 || byte_count != count * 2 || frame_len < 13 + byte_count) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    if (start < WRITABLE_FIRST || start + count - 1 > WRITABLE_LAST) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }
    // Validate every value before writing any, so a rejected request changes nothing
    for (uint16_t i = 0; i < count; i++) {
      uint16_t val = (c.buf[13 + i * 2] << 8) | c.buf[14 + i * 2];
      if (!is_valid_control_value(start + i, val)) {
        this->send_exception_(c, txid, uid, fc, 0x03);
        return;
      }
    }

    bool limit_changed = false;
    for (uint16_t i = 0; i < count; i++) {
      uint16_t addr = start + i;
      this->set_reg_(addr, (c.buf[13 + i * 2] << 8) | c.buf[14 + i * 2]);
      limit_changed |= is_limit_register(addr);
    }
    if (limit_changed)
      this->on_limit_command_();

    uint8_t pdu[5] = {fc, c.buf[8], c.buf[9], c.buf[10], c.buf[11]};
    this->send_response_(c, txid, uid, pdu, sizeof(pdu));

  } else {
    this->send_exception_(c, txid, uid, fc, 0x01);  // Illegal Function
  }
}

void SunspecComponent::send_response_(Client &c, uint16_t txid, uint8_t uid, const uint8_t *pdu, uint16_t pdu_len) {
  // Max frame: 7-byte MBAP header + up to 252-byte PDU = 259 bytes
  uint8_t frame[7 + 2 + 125 * 2];
  frame[0] = txid >> 8;
  frame[1] = txid & 0xFF;
  frame[2] = 0x00;  // Protocol ID
  frame[3] = 0x00;
  frame[4] = (1 + pdu_len) >> 8;
  frame[5] = (1 + pdu_len) & 0xFF;
  frame[6] = uid;
  memcpy(frame + 7, pdu, pdu_len);
  const ssize_t total = 7 + pdu_len;
  ssize_t sent = c.sock->write(frame, total);
  // Send buffer full (nothing written): drop this response rather than block the main
  // loop; the client retries after its own timeout
  if (sent == 0 || (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
    ESP_LOGD(TAG, "Client %u send buffer full, response dropped", this->client_index_(c));
    return;
  }
  if (sent != total) {
    ESP_LOGW(TAG, "Client %u write failed (expected=%d, got=%d), closing", this->client_index_(c),
             static_cast<int>(total), static_cast<int>(sent));
    this->close_client_(c);
  }
}

void SunspecComponent::send_exception_(Client &c, uint16_t txid, uint8_t uid, uint8_t fc, uint8_t code) {
  uint8_t pdu[2] = {static_cast<uint8_t>(fc | 0x80), code};
  this->send_response_(c, txid, uid, pdu, 2);
}

// ---------- power limit ----------

void SunspecComponent::on_limit_command_() {
  // WMaxLimPct_RvrtTms (seconds; 0 or "not implemented" = no revert): every limit
  // command restarts the timer, so the limit lapses only when commands stop arriving
  uint16_t rvrt_s = this->get_reg_(REG_WMAXLIMPCT_RVRTTMS);
  if (this->get_power_limit_enabled() && rvrt_s != 0 && rvrt_s != NI_UINT16) {
    this->revert_start_ms_ = App.get_loop_component_start_time();
    this->revert_after_ms_ = rvrt_s * 1000u;
  } else {
    this->revert_after_ms_ = 0;
  }
  this->apply_power_limit_();
}

void SunspecComponent::apply_power_limit_() {
  const bool enabled = this->get_power_limit_enabled();
  const float pct = this->get_power_limit_pct();
  this->power_limit_callback_.call(pct, enabled);

  // Energy managers re-send the same limit every few seconds; forward only changes (plus a
  // periodic refresh) so the inverter bus isn't flooded and EEPROM-backed limits aren't worn
  const uint32_t limit = enabled ? this->get_reg_(REG_WMAXLIMPCT) : LIMIT_DISABLED;
  const uint32_t now = App.get_loop_component_start_time();
  if (limit == this->applied_limit_ && now - this->applied_at_ms_ < LIMIT_REFRESH_MS) {
    ESP_LOGV(TAG, "Power limit unchanged, not forwarding");
    return;
  }
  if (limit != this->applied_limit_) {
    if (enabled) {
      ESP_LOGI(TAG, "Power limit: %.2f%%", pct);
    } else {
      ESP_LOGI(TAG, "Power limit disabled (full power)");
    }
  }
  this->applied_limit_ = limit;
  this->applied_at_ms_ = now;

#ifdef USE_SUNSPEC_POWER_LIMIT_NUMBER
  if (this->power_limit_number_ != nullptr) {
    // Route through the number entity: 0–100 = limit %, 110 = unlimited
    auto call = this->power_limit_number_->make_call();
    call.set_value(enabled ? pct : 110.0f);
    call.perform();
    return;
  }
#endif
#ifdef USE_SUNSPEC_MODBUS_CONTROLLER
  if (this->controller_ != nullptr) {
    // Direct Modbus register write; 100 restores full power
    uint16_t write_val = enabled ? static_cast<uint16_t>(std::lround(pct)) : 100;
#ifdef USE_SUNSPEC_MODBUS_CLIENT_DEVICE
    if (!this->limit_writer_.write_single_register(this->power_limit_register_, write_val))
      this->on_limit_write_failed_("refused by the Modbus bus (queue full?)");
#else
    auto cmd = modbus_controller::ModbusCommandItem::create_write_single_command(
        this->controller_, this->power_limit_register_, write_val);
    this->controller_->queue_command(cmd);
#endif
    return;
  }
#endif
  // Trigger-only setups are valid; on_power_limit automations were already fired above
}

#ifdef USE_SUNSPEC_MODBUS_CLIENT_DEVICE
void SunspecComponent::on_limit_write_failed_(const char *reason) {
  ESP_LOGW(TAG, "Power limit write to register %u %s; will retry on the next limit command",
           this->power_limit_register_, reason);
  this->applied_limit_ = LIMIT_UNKNOWN;
}

void SunspecComponent::LimitWriter::on_error(std::span<const uint8_t> request_pdu,
                                             modbus::ExceptionCode exception_code) {
  this->component_->on_limit_write_failed_("was rejected by the inverter");
}

bool SunspecComponent::LimitWriter::on_no_response(std::span<const uint8_t> request_pdu) {
  this->component_->on_limit_write_failed_("got no response");
  return false;  // no immediate retry; the next limit command re-sends
}

void SunspecComponent::LimitWriter::on_not_sent(std::span<const uint8_t> request_pdu) {
  this->component_->on_limit_write_failed_("was dropped before sending");
}
#endif

}  // namespace esphome::sunspec
