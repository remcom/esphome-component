#include "sunspec.h"

#include <cinttypes>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome::sunspec {

static const char *const TAG = "sunspec";

// ---------- helpers ----------

uint16_t SunspecComponent::to_sf(float val, int sf) {
  if (std::isnan(val)) return NI_INT16;
  float scaled = val * std::pow(10.0f, (float)(-sf));
  if (scaled > 32767.0f)  return (uint16_t) 32767;
  if (scaled < -32768.0f) return (uint16_t)(int16_t) -32768;
  return (uint16_t)(int16_t)(scaled + (scaled >= 0 ? 0.5f : -0.5f));
}

uint16_t SunspecComponent::to_u16_sf(float val, int sf) {
  if (std::isnan(val)) return NI_UINT16;
  float scaled = val * std::pow(10.0f, (float)(-sf));
  // Cap below the 0xFFFF "not implemented" sentinel
  if (scaled < 0.0f)     return 0;
  if (scaled > 65534.0f) return 65534;
  return (uint16_t)(scaled + 0.5f);
}

float SunspecComponent::fresh_state_(uint8_t slot) {
  sensor::Sensor *s = this->sensors_[slot];
  if (s == nullptr || !s->has_state()) return NAN;
  if (this->stale_timeout_ms_ > 0 &&
      millis() - this->sensor_last_update_[slot] > this->stale_timeout_ms_) {
    return NAN;
  }
  return s->get_state();
}

void SunspecComponent::encode_string_(uint16_t *dest, const std::string &s, uint8_t reg_count) {
  // Pack ASCII bytes as big-endian uint16 pairs, null-padded.
  // reg_count is at most 16, so 32 bytes is a safe fixed upper bound.
  uint8_t buf[32];
  memset(buf, 0, reg_count * 2);
  size_t copy_len = std::min(s.size(), (size_t)(reg_count * 2));
  memcpy(buf, s.c_str(), copy_len);
  for (uint8_t i = 0; i < reg_count; i++) {
    dest[i] = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
  }
}

// ---------- static register map ----------

void SunspecComponent::init_static_registers_() {
  // Initialise entire bank to 0xFFFF (SunSpec uint16 "not implemented")
  for (uint16_t i = 0; i < REG_COUNT; i++) this->registers_[i] = NI_UINT16;

  // --- Common Block (Model 1) ---
  this->set_reg(40000, 0x5375);  // SunS marker high
  this->set_reg(40001, 0x6e53);  // SunS marker low
  this->set_reg(40002, 1);       // Model ID
  this->set_reg(40003, 66);      // Length (data registers only)

  this->encode_string_(&this->registers_[4],  this->manufacturer_,  16);  // 40004-40019
  this->encode_string_(&this->registers_[20], this->model_,         16);  // 40020-40035
  this->encode_string_(&this->registers_[36], "",                    8);  // 40036-40043 (options, empty)
  this->encode_string_(&this->registers_[44], this->version_,        8);  // 40044-40051
  this->encode_string_(&this->registers_[52], this->serial_number_, 16);  // 40052-40067

  this->set_reg(40068, this->unit_address_);  // DA (Modbus device address)
  this->set_reg(40069, 0);                    // Padding

  // --- Inverter Block (Model 101 single phase / 103 three phase) ---
  this->set_reg(40070, this->model_id_);  // Model ID
  this->set_reg(40071, 50);               // Length
  // 40072-40075: A/AphA/AphB/AphC -- updated in update_slot_()
  bool has_current = this->has_current_source_() || this->sensors_[SLOT_AC_CURRENT_PHA] ||
                     this->sensors_[SLOT_AC_CURRENT_PHB] || this->sensors_[SLOT_AC_CURRENT_PHC];
  this->set_reg(40076, has_current ? (uint16_t)(int16_t)(-2) : NI_INT16);  // A_SF
  // 40077-40079: phase-to-phase voltage AB/BC/CA -- stay NI
  // 40080-40082: PhVphA/BN/CN -- updated in update_slot_() where configured
  this->set_reg(40083, (uint16_t)(int16_t)(-1));  // V_SF = -1
  this->set_reg(40084, NI_INT16);                 // W -- updated in update_slot_()
  this->set_reg(40085, 0);                        // W_SF = 0
  // 40086: Hz -- updated in update_slot_()
  this->set_reg(40087, (uint16_t)(int16_t)(-2));  // Hz_SF = -2
  // 40088-40093: VA/VAr/PF and their SFs -- int16 "not implemented"
  for (uint16_t addr = 40088; addr <= 40093; addr++) this->set_reg(addr, NI_INT16);
  this->set_reg(40094, 0);  // WH high -- updated in update_slot_()
  this->set_reg(40095, 0);  // WH low
  this->set_reg(40096, 0);  // WH_SF = 0
  this->set_reg(40097, NI_UINT16);  // DCA -- updated in update_slot_() if configured
  this->set_reg(40098, this->sensors_[SLOT_DC_CURRENT] ? (uint16_t)(int16_t)(-2) : NI_INT16);  // DCA_SF
  this->set_reg(40099, NI_UINT16);  // DCV
  this->set_reg(40100, this->sensors_[SLOT_DC_VOLTAGE] ? (uint16_t)(int16_t)(-1) : NI_INT16);  // DCV_SF
  this->set_reg(40101, NI_INT16);   // DCW
  this->set_reg(40102, this->sensors_[SLOT_DC_POWER] ? (uint16_t) 0 : NI_INT16);  // DCW_SF
  this->set_reg(40103, NI_INT16);  // TmpCab -- updated in update_slot_()
  this->set_reg(40104, NI_INT16);  // TmpSnk
  this->set_reg(40105, NI_INT16);  // TmpTrns
  this->set_reg(40106, NI_INT16);  // TmpOt
  this->set_reg(40107, (uint16_t)(int16_t)(-1));  // Tmp_SF = -1
  this->set_reg(40108, ST_OFF);    // St (updated in update_slot_())
  // 40109: StVnd -- stays NI
  // 40110-40121: Evt1/Evt2/EvtVnd1-4 = 0 (no events)
  for (uint16_t addr = 40110; addr <= 40121; addr++) this->set_reg(addr, 0);

  // --- Nameplate Block (Model 120) ---
  this->set_reg(40122, 120);  // Model ID
  this->set_reg(40123, 26);   // Length
  this->set_reg(40124, 4);    // DER type = PV
  this->set_reg(40125, this->rated_power_);  // WRtg (watts)
  this->set_reg(40126, 0);    // WRtg_SF = 0
  // 40127-40149: stay NI

  // --- Immediate Controls Block (Model 123) ---
  this->set_reg(40150, 123);        // Model ID
  this->set_reg(40151, 24);         // Length
  // 40152-40153: Conn_WinTms / Conn_RvrtTms -- stay NI
  this->set_reg(40154, 1);          // Conn = connected
  this->set_reg(40155, 10000);      // WMaxLimPct default = 100.00 % (SF=-2)
  // 40156-40158: WMaxLimPct WinTms/RvrtTms/RmpTms -- stay NI
  this->set_reg(40159, 0);          // WMaxLim_Ena = 0 (disabled)
  this->set_reg(40160, NI_INT16);   // OutPFSet
  // 40161-40163: OutPFSet WinTms/RvrtTms/RmpTms -- stay NI
  this->set_reg(40164, 0);          // OutPFSet_Ena = 0 (disabled)
  this->set_reg(40165, NI_INT16);   // VArWMaxPct
  this->set_reg(40166, NI_INT16);   // VArMaxPct
  this->set_reg(40167, NI_INT16);   // VArAvalPct
  // 40168-40170: VArPct WinTms/RvrtTms/RmpTms -- stay NI
  // 40171: VArPct_Mod -- stays NI
  this->set_reg(40172, 0);          // VArPct_Ena = 0 (disabled)
  this->set_reg(40173, (uint16_t)(int16_t)(-2));  // WMaxLimPct_SF = -2 (0.01 %)
  this->set_reg(40174, NI_INT16);   // OutPFSet_SF
  this->set_reg(40175, NI_INT16);   // VArPct_SF

  // --- End Model (ID 0xFFFF, length 0) ---
  this->set_reg(40176, 0xFFFF);
  this->set_reg(40177, 0);
}

// ---------- lifecycle ----------

void SunspecComponent::setup() {
  // 1. Validate required sensor pointers
  if (!this->sensors_[SLOT_AC_POWER] || !this->sensors_[SLOT_AC_VOLTAGE] ||
      !this->sensors_[SLOT_AC_FREQUENCY] || !this->sensors_[SLOT_TEMPERATURE]) {
    ESP_LOGE(TAG, "One or more required sensors are not configured");
    this->mark_failed();
    return;
  }

  // 2. Event-driven register updates: each sensor publish patches only its own
  //    register slot(s). The timestamps let check_stale_slots_() report sensors
  //    that stop updating as "not implemented" instead of serving frozen data.
  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    sensor::Sensor *s = this->sensors_[slot];
    if (s == nullptr) continue;
    this->sensor_last_update_[slot] = millis();
    s->add_on_state_callback([this, slot](float) {
      this->sensor_last_update_[slot] = millis();
      this->slot_fresh_[slot] = true;
      this->update_slot_(slot);
    });
  }

  // 3. Initialise register bank, then seed live slots from sensors that
  //    already have a state (e.g. restored values)
  this->init_static_registers_();
  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    if (this->sensors_[slot] == nullptr) continue;
    if (this->sensors_[slot]->has_state()) this->slot_fresh_[slot] = true;
    this->update_slot_(slot);
  }

  // 4. Open non-blocking TCP socket
  this->server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->server_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed: %d", errno);
    this->mark_failed();
    return;
  }

  int opt = 1;
  ::setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons(this->port_);

  if (::bind(this->server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed on port %u: %d", this->port_, errno);
    ::close(this->server_fd_);
    this->server_fd_ = -1;
    this->mark_failed();
    return;
  }

  if (::listen(this->server_fd_, this->max_connections_) < 0) {
    ESP_LOGE(TAG, "listen() failed: %d", errno);
    ::close(this->server_fd_);
    this->server_fd_ = -1;
    this->mark_failed();
    return;
  }

  // Set non-blocking
  int flags = ::fcntl(this->server_fd_, F_GETFL, 0);
  if (flags < 0 || ::fcntl(this->server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ESP_LOGE(TAG, "fcntl() failed: %d", errno);
    ::close(this->server_fd_);
    this->server_fd_ = -1;
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "SunSpec Modbus TCP server listening on port %u", this->port_);
}

void SunspecComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SunSpec Modbus TCP:");
  ESP_LOGCONFIG(TAG, "  Manufacturer:    %s", this->manufacturer_.c_str());
  ESP_LOGCONFIG(TAG, "  Model:           %s", this->model_.c_str());
  ESP_LOGCONFIG(TAG, "  Serial:          %s", this->serial_number_.c_str());
  ESP_LOGCONFIG(TAG, "  Version:         %s", this->version_.c_str());
  ESP_LOGCONFIG(TAG, "  Rated Power:     %u W", this->rated_power_);
  ESP_LOGCONFIG(TAG, "  Inverter Model:  %u", this->model_id_);
  ESP_LOGCONFIG(TAG, "  Port:            %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Max Connections: %u", this->max_connections_);
  ESP_LOGCONFIG(TAG, "  Unit Address:    %u", this->unit_address_);
  ESP_LOGCONFIG(TAG, "  Stale Timeout:   %" PRIu32 " ms", this->stale_timeout_ms_);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed!");
  }
}

void SunspecComponent::loop() {
  if (this->server_fd_ < 0) return;

  this->accept_clients_();
  this->check_stale_slots_();

  for (auto &c : this->clients_) {
    if (c.fd >= 0) this->process_client_(c);
  }
}

void SunspecComponent::accept_clients_() {
  struct sockaddr_in client_addr{};
  socklen_t addr_len = sizeof(client_addr);
  int fd = ::accept(this->server_fd_, (struct sockaddr *)&client_addr, &addr_len);
  if (fd < 0) return;  // EAGAIN / EWOULDBLOCK -- no pending connection

  // Find a free slot within the configured connection limit
  for (uint8_t i = 0; i < this->max_connections_ && i < MAX_CLIENTS; i++) {
    Client &c = this->clients_[i];
    if (c.fd < 0) {
      int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "fcntl() failed for client (fd=%d): %d", fd, errno);
        ::close(fd);
        return;
      }
      // Small responses to poll requests should not wait for Nagle
      int opt = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
      ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
      c.fd           = fd;
      c.buf_len      = 0;
      c.last_recv_ms = millis();
      ESP_LOGD(TAG, "Client connected (fd=%d) from %s", fd, inet_ntoa(client_addr.sin_addr));
      return;
    }
  }

  // No free slot -- reject
  ESP_LOGW(TAG, "Max clients reached, rejecting connection");
  ::close(fd);
}

void SunspecComponent::close_client_(Client &c) {
  if (c.fd >= 0) {
    ESP_LOGD(TAG, "Closing client (fd=%d)", c.fd);
    ::close(c.fd);
    c.fd      = -1;
    c.buf_len = 0;
  }
}

// ---------- sensor updates (event-driven) ----------

void SunspecComponent::update_slot_(uint8_t slot) {
  switch (slot) {
    case SLOT_AC_POWER: {
      // AC power (SF=0): direct watts
      float pwr = this->fresh_state_(SLOT_AC_POWER);
      this->set_reg(40084, to_sf(pwr, 0));
      // Inverter state: no fresh power data -> Off; producing -> MPPT; else Sleeping
      uint16_t st = ST_OFF;
      if (!std::isnan(pwr)) st = (pwr > 5.0f) ? ST_MPPT : ST_SLEEPING;
      this->set_reg(40108, st);
      // Derived current depends on power
      if (this->sensors_[SLOT_AC_CURRENT] == nullptr && this->model_id_ == 101)
        this->update_slot_(SLOT_AC_CURRENT);
      break;
    }
    case SLOT_AC_VOLTAGE: {
      // AC voltage phase A (SF=-1): value x 10
      this->set_reg(40080, to_u16_sf(this->fresh_state_(SLOT_AC_VOLTAGE), -1));
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
        float pwr  = this->fresh_state_(SLOT_AC_POWER);
        float volt = this->fresh_state_(SLOT_AC_VOLTAGE);
        if (!std::isnan(pwr) && !std::isnan(volt) && volt > 1.0f) curr = pwr / volt;
      }
      uint16_t raw = to_u16_sf(curr, -2);
      this->set_reg(40072, raw);                          // A (total)
      if (this->model_id_ == 101) this->set_reg(40073, raw);  // AphA mirrors total
      break;
    }
    case SLOT_AC_CURRENT_PHA:
      this->set_reg(40073, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_CURRENT_PHB:
      this->set_reg(40074, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_CURRENT_PHC:
      this->set_reg(40075, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_AC_VOLTAGE_PHB:
      this->set_reg(40081, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_AC_VOLTAGE_PHC:
      this->set_reg(40082, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_AC_FREQUENCY:
      // AC frequency (SF=-2): value x 100
      this->set_reg(40086, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    case SLOT_TEMPERATURE:
      // Temperature (SF=-1): value x 10
      this->set_reg(40103, to_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_ENERGY: {
      // Energy (acc32, SF=0, sensor reports kWh). Accumulators keep their last
      // known total when the sensor goes stale, so bypass the staleness check.
      sensor::Sensor *energy = this->sensors_[SLOT_ENERGY];
      if (energy != nullptr && energy->has_state()) {
        float e = energy->get_state();
        if (!std::isnan(e) && e >= 0) {
          uint32_t wh = (uint32_t)(e * 1000.0f);
          this->set_reg(40094, (uint16_t)(wh >> 16));
          this->set_reg(40095, (uint16_t)(wh & 0xFFFF));
        }
      }
      break;
    }
    case SLOT_DC_POWER:
      if (this->sensors_[slot]) this->set_reg(40101, to_sf(this->fresh_state_(slot), 0));
      break;
    case SLOT_DC_VOLTAGE:
      if (this->sensors_[slot]) this->set_reg(40099, to_u16_sf(this->fresh_state_(slot), -1));
      break;
    case SLOT_DC_CURRENT:
      if (this->sensors_[slot]) this->set_reg(40097, to_u16_sf(this->fresh_state_(slot), -2));
      break;
    default:
      break;
  }
}

void SunspecComponent::check_stale_slots_() {
  if (this->stale_timeout_ms_ == 0) return;
  uint32_t now = millis();
  if (now - this->last_stale_check_ < 1000) return;
  this->last_stale_check_ = now;

  for (uint8_t slot = 0; slot < SLOT_COUNT; slot++) {
    // Energy is an accumulator; the last known total stays valid
    if (this->sensors_[slot] == nullptr || slot == SLOT_ENERGY) continue;
    if (!this->slot_fresh_[slot]) continue;
    if (now - this->sensor_last_update_[slot] < this->stale_timeout_ms_) continue;

    this->slot_fresh_[slot] = false;
    this->update_slot_(slot);  // fresh_state_() now returns NaN -> "not implemented"
    ESP_LOGW(TAG, "Sensor '%s' has not updated for %" PRIu32 " ms; reporting as not implemented",
             this->sensors_[slot]->get_name().c_str(), this->stale_timeout_ms_);
  }
}

// ---------- TCP framing ----------

void SunspecComponent::process_client_(Client &c) {
  // Idle timeout check
  if (millis() - c.last_recv_ms > IDLE_TIMEOUT_MS) {
    ESP_LOGD(TAG, "Client idle timeout (fd=%d)", c.fd);
    this->close_client_(c);
    return;
  }

  // Read available bytes
  int n = ::recv(c.fd, c.buf + c.buf_len, MAX_BUF - c.buf_len, 0);
  if (n == 0) { this->close_client_(c); return; }
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) this->close_client_(c);
    return;
  }
  c.buf_len     += n;
  c.last_recv_ms = millis();

  // Overflow guard
  if (c.buf_len >= MAX_BUF) {
    ESP_LOGD(TAG, "Client buffer overflow (fd=%d), closing", c.fd);
    this->close_client_(c);
    return;
  }

  // Process all complete frames in buffer
  while (c.buf_len >= 6) {
    uint16_t proto_id   = (c.buf[2] << 8) | c.buf[3];
    uint16_t pdu_length = (c.buf[4] << 8) | c.buf[5];  // bytes after 6-byte MBAP header

    // Validate protocol ID
    if (proto_id != 0x0000) {
      ESP_LOGD(TAG, "Invalid protocol ID %04X, closing", proto_id);
      this->close_client_(c);
      return;
    }

    uint16_t frame_len = 6 + pdu_length;  // total frame bytes
    if (frame_len > MAX_BUF) {
      ESP_LOGD(TAG, "Frame too large (%u bytes), closing", frame_len);
      this->close_client_(c);
      return;
    }

    if (c.buf_len < frame_len) break;  // incomplete frame, wait for more data

    this->handle_frame_(c, frame_len);
    if (c.fd < 0) return;  // handler closed the connection

    // Consume processed frame from buffer
    c.buf_len -= frame_len;
    if (c.buf_len > 0) memmove(c.buf, c.buf + frame_len, c.buf_len);
  }
}

void SunspecComponent::handle_frame_(Client &c, uint16_t frame_len) {
  uint16_t txid = (c.buf[0] << 8) | c.buf[1];
  uint8_t  uid  = c.buf[6];
  uint8_t  fc   = c.buf[7];

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
    if (frame_len < 12) { this->send_exception_(c, txid, uid, fc, 0x03); return; }
    uint16_t start = (c.buf[8]  << 8) | c.buf[9];
    uint16_t count = (c.buf[10] << 8) | c.buf[11];

    if (count == 0 || count > 125) { this->send_exception_(c, txid, uid, fc, 0x03); return; }
    if (start < BASE_ADDR || (start + count) > (BASE_ADDR + REG_COUNT)) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }

    // Max count is 125 registers → 2 + 250 = 252 bytes
    uint8_t pdu[252];
    pdu[0] = fc;
    pdu[1] = count * 2;
    uint16_t idx = start - BASE_ADDR;
    for (uint16_t i = 0; i < count; i++) {
      pdu[2 + i * 2]     = this->registers_[idx + i] >> 8;
      pdu[2 + i * 2 + 1] = this->registers_[idx + i] & 0xFF;
    }
    this->send_response_(c, txid, uid, pdu, 2 + count * 2);

  } else if (fc == 0x06) {
    // FC06: Write Single Register
    if (frame_len < 12) { this->send_exception_(c, txid, uid, fc, 0x03); return; }
    uint16_t addr = (c.buf[8]  << 8) | c.buf[9];
    uint16_t val  = (c.buf[10] << 8) | c.buf[11];

    if (addr < WRITABLE_FIRST || addr > WRITABLE_LAST) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }
    this->set_reg(addr, val);
    if (addr == REG_WMAXLIMPCT || addr == REG_WMAXLIM_ENA) this->apply_power_limit_();

    // Echo back per Modbus spec
    uint8_t pdu[5] = { fc, c.buf[8], c.buf[9], c.buf[10], c.buf[11] };
    this->send_response_(c, txid, uid, pdu, sizeof(pdu));

  } else if (fc == 0x10) {
    // FC16: Write Multiple Registers
    if (frame_len < 13) { this->send_exception_(c, txid, uid, fc, 0x03); return; }
    uint16_t start      = (c.buf[8]  << 8) | c.buf[9];
    uint16_t count      = (c.buf[10] << 8) | c.buf[11];
    uint8_t  byte_count = c.buf[12];

    if (count == 0 || count > 123 || byte_count != count * 2 ||
        frame_len < (uint16_t)(13 + byte_count)) {
      this->send_exception_(c, txid, uid, fc, 0x03);
      return;
    }
    if (start < WRITABLE_FIRST || (start + count - 1) > WRITABLE_LAST) {
      this->send_exception_(c, txid, uid, fc, 0x02);
      return;
    }

    bool limit_changed = false;
    for (uint16_t i = 0; i < count; i++) {
      uint16_t addr = start + i;
      this->set_reg(addr, (c.buf[13 + i * 2] << 8) | c.buf[14 + i * 2]);
      if (addr == REG_WMAXLIMPCT || addr == REG_WMAXLIM_ENA) limit_changed = true;
    }
    if (limit_changed) this->apply_power_limit_();

    uint8_t pdu[5] = { fc, c.buf[8], c.buf[9], c.buf[10], c.buf[11] };
    this->send_response_(c, txid, uid, pdu, sizeof(pdu));

  } else {
    this->send_exception_(c, txid, uid, fc, 0x01);  // Illegal Function
  }
}

void SunspecComponent::send_response_(Client &c, uint16_t txid, uint8_t uid,
                                      const uint8_t *pdu, uint16_t pdu_len) {
  // Max frame: 7-byte MBAP header + up to 252-byte PDU = 259 bytes
  uint8_t frame[259];
  frame[0] = txid >> 8;   frame[1] = txid & 0xFF;
  frame[2] = 0x00;        frame[3] = 0x00;  // Protocol ID
  frame[4] = (1 + pdu_len) >> 8;
  frame[5] = (1 + pdu_len) & 0xFF;
  frame[6] = uid;
  memcpy(frame + 7, pdu, pdu_len);
  uint16_t frame_len = 7 + pdu_len;

  // Handle partial writes on the non-blocking socket
  uint16_t offset  = 0;
  uint8_t  retries = 0;
  while (offset < frame_len) {
    int sent = ::send(c.fd, frame + offset, frame_len - offset, 0);
    if (sent > 0) {
      offset += sent;
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (++retries > 50) {
        ESP_LOGW(TAG, "send() timed out (fd=%d)", c.fd);
        this->close_client_(c);
        return;
      }
      delay(1);
      continue;
    }
    ESP_LOGW(TAG, "send() failed (fd=%d): %d", c.fd, errno);
    this->close_client_(c);
    return;
  }
}

void SunspecComponent::send_exception_(Client &c, uint16_t txid, uint8_t uid,
                                       uint8_t fc, uint8_t code) {
  uint8_t pdu[2] = { (uint8_t)(fc | 0x80), code };
  this->send_response_(c, txid, uid, pdu, 2);
}

void SunspecComponent::apply_power_limit_() {
  bool  enabled = this->get_power_limit_enabled();
  float pct     = this->get_power_limit_pct();
  if (pct > 100.0f) pct = 100.0f;

  ESP_LOGI(TAG, "Power limit command: %.2f%% (%s)", pct, enabled ? "enabled" : "disabled");
  this->power_limit_callback_.call(pct, enabled);

  if (this->power_limit_number_ != nullptr) {
    // Route through the number entity: 0–100 = limit %, 110 = unlimited
    auto call = this->power_limit_number_->make_call();
    call.set_value(enabled ? pct : 110.0f);
    call.perform();
    return;
  }

  if (this->controller_ != nullptr) {
    // Direct Modbus register write; 100 restores full power
    uint16_t write_val = enabled ? (uint16_t)(pct + 0.5f) : 100;
    auto cmd = modbus_controller::ModbusCommandItem::create_write_single_command(
        this->controller_, this->power_limit_register_, write_val);
    this->controller_->queue_command(cmd);
    return;
  }

  // Trigger-only setups are valid; on_power_limit automations were already fired above
  ESP_LOGD(TAG, "No number entity or ModbusController configured for power limit write-back");
}

}  // namespace esphome::sunspec
