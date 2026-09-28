#include "pacific_fan.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "soc/gpio_struct.h"

namespace esphome::pacific_fan {

static const char *const TAG = "pacific_fan";

static const float FAN_MHZ = 433.92f;

// ── Pacific timings (averages over ~45 captured bursts) ─────
static const int SHORT_US = 375;
static const int LONG_US = 1090;
static const int GAP_US = 5600;
static const int TX_REPS = 10;  // the remote sends ~9 per keying

// ── NEC timings (WMT202-RS remote, ~20 presses) ─────────────
static const int NEC_LEAD_MARK_US = 9000;   // NEC's leader, as on infrared
static const int NEC_LEAD_SPACE_US = 4500;
static const int NEC_MARK_US = 560;
static const int NEC_ZERO_US = 560;   // space
static const int NEC_ONE_US = 1650;   // space
static const int NEC_GAP_US = 20000;
static const int NEC_TX_REPS = 10;    // the remote sends ~17 per press; ~0.7s is plenty
static const uint32_t NEC_BURST_GAP_MS = 250;  // back-to-back bursts would read as one long press

// ── RX decoder tolerances ───────────────────────────────────
static const uint16_t MARK_MIN = 150;
static const uint16_t MARK_SPLIT = 700;  // below = '0', above = '1'
static const uint16_t MARK_MAX = 1800;
static const uint16_t GAP_MIN = 3500;
static const uint16_t NEC_MARK_MIN = 300;
static const uint16_t NEC_MARK_MAX = 900;
static const uint16_t NEC_SPACE_SPLIT = 1100;  // below = '0', above = '1'
static const uint16_t NEC_SPACE_MAX = 2300;
static const uint32_t PRESS_GAP_MS = 500;  // same code closer than this = same press

// ── Learn Mode, for remotes neither decoder knows ───────────
static const uint16_t SNIFF_MIN_US = 100;   // shorter is receiver noise
static const uint16_t SNIFF_GAP_US = 2500;  // a space this long ends a packet
static const int SNIFF_MIN_PULSES = 24;
static const int RSSI_RISE_DB = 15;           // above the noise floor = someone is transmitting
static const uint32_t RSSI_QUIET_MS = 300;    // this long back at the floor ends it

// ── Behaviour ───────────────────────────────────────────────
static const int DIM_OVERSHOOT = 2;           // extra presses at either end of the dimmer
static const uint32_t LIGHT_MIN_GAP_MS = 3000;  // a fast off/on changes the light's colour

const char *command_name(const Commands &cmds, uint16_t cmd) {
  if (cmd == cmds.toggle) return "Power";
  if (cmd == cmds.breeze) return "Breeze";
  if (cmd == cmds.reverse) return "F/R";
  if (cmd == cmds.timer_1h) return "Timer 1H";
  if (cmd == cmds.timer_4h) return "Timer 4H";
  if (cmd == cmds.timer_8h) return "Timer 8H";
  if (cmd == cmds.light) return "Light";
  if (cmd == cmds.colour) return "Light colour";
  if (cmd == cmds.dim_down) return "LED-";
  if (cmd == cmds.dim_up) return "LED+";
  static const char *const SPEEDS[] = {"Speed 1", "Speed 2", "Speed 3", "Speed 4", "Speed 5", "Speed 6"};
  int s = speed_of(cmds, cmd);
  return s ? SPEEDS[s - 1] : "?";
}

int speed_of(const Commands &cmds, uint16_t cmd) {
  for (int i = 0; i < 6; i++)
    if (cmds.speed[i] == cmd) return i + 1;
  return 0;
}

// ============================================================
// ISR: edge durations off GDO2 into a ring buffer
// ============================================================
// A press is ~1100 edges and the loop may be busy (our own TX, an API
// call), so the ring holds several presses.
static const uint32_t RING = 4096;  // power of two
static volatile uint16_t ring_buf[RING];  // bit15 set = the pulse that ended was a mark
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_last_us = 0;
static uint8_t ring_pin = 0;

// digitalRead() lives in flash and is not safe from an IRAM ISR.
static inline bool IRAM_ATTR read_pin(uint8_t pin) {
  if (pin < 32) return (GPIO.in >> pin) & 0x1;
  return (GPIO.in1.val >> (pin - 32)) & 0x1;
}

static void IRAM_ATTR rx_isr() {
  uint32_t now = micros();
  uint32_t dt = now - ring_last_us;
  ring_last_us = now;
  if (dt > 0x7FFF) dt = 0x7FFF;
  uint16_t v = (uint16_t) dt;
  if (!read_pin(ring_pin)) v |= 0x8000;  // now LOW -> it was a HIGH mark
  ring_buf[ring_head & (RING - 1)] = v;
  ring_head++;
}

// ============================================================
// PacificFanRadio
// ============================================================
void PacificFanRadio::setup() {
  this->radio_ = new CC1101(this->sck_, this->miso_, this->mosi_, this->cs_, this->gdo0_, this->gdo2_);  // NOLINT
  this->radio_->init();
  this->radio_->setMHZ(FAN_MHZ);
  this->radio_->setTXPwr(TX_PLUS_10_DBM);
  this->radio_->setDataRate(2400);
  this->radio_->setRxBW(RX_BW_162_KHZ);
  this->radio_->setModulation(ASK_OOK);

  uint8_t version = this->radio_->getVersion();
  if (version == 0x00 || version == 0xFF) {
    ESP_LOGE(TAG, "CC1101 not detected (version 0x%02X) - check the wiring", version);
    this->mark_failed();
    return;
  }

  // The library only drives GDO0 as an output when built without GDO2.
  pinMode(this->gdo0_, OUTPUT);
  digitalWrite(this->gdo0_, LOW);
  ring_pin = this->gdo2_;
  pinMode(this->gdo2_, INPUT);

  this->tuned_ = 0;
  this->ready_ = true;
  this->start_rx_();
}

void PacificFanRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "Pacific fan radio (CC1101 @ 433.92 MHz OOK)");
  ESP_LOGCONFIG(TAG, "  SCK %d  MISO %d  MOSI %d  CS %d  GDO0 %d  GDO2 %d", this->sck_, this->miso_, this->mosi_,
                this->cs_, this->gdo0_, this->gdo2_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  CC1101 not detected");
}

void PacificFanRadio::tune_(size_t index) {
  if (index >= this->tunings_.size())
    index = 0;
  if (index == this->tuned_)
    return;
  Tuning t = index < this->tunings_.size() ? this->tunings_[index] : Tuning{FAN_MHZ, false};
  this->radio_->setIdle();
  this->radio_->setMHZ(t.mhz);
  this->radio_->setModulation(t.fsk ? FSK2 : ASK_OOK);
  // Off the fans' own tuning, listen wide: an unknown remote may sit well
  // off its nominal frequency.
  this->radio_->setRxBW(index == 0 ? RX_BW_162_KHZ : RX_BW_325_KHZ);
  this->tuned_ = index;
}

void PacificFanRadio::set_learn_mode(bool on) {
  this->learn_mode_ = on;
  this->sniff_n_ = 0;
  this->sniff_reps_ = 0;
  this->rssi_active_ = false;
  this->rssi_floor_ = 0;
  if (!this->ready_)
    return;
  this->tune_(on ? this->tuning_ : 0);
  this->radio_->setRx();
  if (on) {
    Tuning t = this->tuned_ < this->tunings_.size() ? this->tunings_[this->tuned_] : Tuning{FAN_MHZ, false};
    ESP_LOGI(TAG, "LEARN on, listening at %.3f MHz %s", t.mhz, t.fsk ? "FSK" : "OOK");
  }
}

void PacificFanRadio::set_tuning(size_t index) {
  this->tuning_ = index;
  this->rssi_floor_ = 0;  // a new frequency has its own noise floor
  if (this->learn_mode_)
    this->set_learn_mode(true);
}

void PacificFanRadio::start_rx_() {
  this->tune_(this->learn_mode_ ? this->tuning_ : 0);
  this->radio_->setRx();
  ring_last_us = micros();
  this->tail_ = ring_head;
  this->state_ = WAIT_GAP;
  attachInterrupt(digitalPinToInterrupt(this->gdo2_), rx_isr, CHANGE);
}

void PacificFanRadio::send(uint32_t address, uint16_t command) {
  PacificRemote *remote = this->find_(address);
  const char *name = command_name(remote ? remote->commands() : PACIFIC_COMMANDS, command);
  if (!this->ready_) {
    ESP_LOGW(TAG, "Radio not ready - dropping %s", name);
    return;
  }
  if ((uint8_t) (this->txq_head_ - this->txq_tail_) >= TXQ) {
    ESP_LOGW(TAG, "TX queue full - dropping %s", name);
    return;
  }
  this->txq_[this->txq_head_++ % TXQ] = {address, command};
}

void PacificFanRadio::transmit_(uint32_t address, uint16_t command) {
  // An address no fan is configured with (the raw sender) goes out as Pacific.
  PacificRemote *remote = this->find_(address);
  Protocol protocol = remote ? remote->protocol() : PACIFIC;

  // Stop listening so we do not decode our own transmission.
  detachInterrupt(digitalPinToInterrupt(this->gdo2_));
  this->tune_(0);  // Learn Mode may be listening elsewhere
  this->radio_->setTx();
  digitalWrite(this->gdo0_, LOW);

  if (protocol == NEC)
    this->transmit_nec_(address, command);
  else
    this->transmit_pacific_(address, command, remote ? remote->even_parity() : true);

  digitalWrite(this->gdo0_, LOW);
  this->start_rx_();
  ESP_LOGI(TAG, protocol == NEC ? "TX  0x%04X  %s" : "TX  0x%05X  %s", (unsigned) address,
           command_name(remote ? remote->commands() : PACIFIC_COMMANDS, command));
}

void PacificFanRadio::transmit_pacific_(uint32_t address, uint16_t command, bool even) {
  uint32_t msg = ((address & 0xFFFFF) << 9) | (command & 0x1FF);
  bool odd_ones = __builtin_popcount(msg) & 1;
  bool check = even ? odd_ones : !odd_ones;
  uint32_t frame = (msg << 1) | (check ? 1 : 0);

  delayMicroseconds(GAP_US);

  for (int rep = 0; rep < TX_REPS; rep++) {
    {
      InterruptLock lock;
      for (int b = 29; b >= 0; b--) {
        bool one = (frame >> b) & 1;
        digitalWrite(this->gdo0_, HIGH);
        delayMicroseconds(one ? LONG_US : SHORT_US);
        digitalWrite(this->gdo0_, LOW);
        delayMicroseconds(one ? SHORT_US : LONG_US);
      }
    }
    delayMicroseconds(GAP_US);
    App.feed_wdt();
  }
}

void PacificFanRadio::transmit_nec_(uint32_t address, uint16_t command) {
  uint8_t cmd = command & 0xFF;
  uint32_t frame = ((address & 0xFFFF) << 16) | ((uint32_t) cmd << 8) | (uint8_t) ~cmd;

  delayMicroseconds(GAP_US);
  for (int rep = 0; rep < NEC_TX_REPS; rep++) {
    digitalWrite(this->gdo0_, HIGH);
    delayMicroseconds(NEC_LEAD_MARK_US);
    digitalWrite(this->gdo0_, LOW);
    delayMicroseconds(NEC_LEAD_SPACE_US);
    {
      InterruptLock lock;
      for (int b = 31; b >= 0; b--) {
        digitalWrite(this->gdo0_, HIGH);
        delayMicroseconds(NEC_MARK_US);
        digitalWrite(this->gdo0_, LOW);
        delayMicroseconds((frame >> b) & 1 ? NEC_ONE_US : NEC_ZERO_US);
      }
      digitalWrite(this->gdo0_, HIGH);  // closing mark
      delayMicroseconds(NEC_MARK_US);
      digitalWrite(this->gdo0_, LOW);
    }
    delay(NEC_GAP_US / 1000);
    App.feed_wdt();
  }
  this->tx_quiet_until_ms_ = millis() + NEC_BURST_GAP_MS;
}

void PacificFanRadio::loop() {
  if (!this->ready_)
    return;
  uint32_t h = ring_head;
  if (h - this->tail_ > RING) {  // overrun: resync on the next gap
    this->tail_ = h - RING;
    this->state_ = WAIT_GAP;
  }
  while (this->tail_ != h) {
    uint16_t v = ring_buf[this->tail_ & (RING - 1)];
    this->tail_++;
    if (this->learn_mode_)
      this->hist_[this->hist_n_++ & (HIST - 1)] = (v & 0x8000) ? (int16_t) (v & 0x7FFF) : -(int16_t) (v & 0x7FFF);
    this->feed_(v & 0x7FFF, (v & 0x8000) != 0);
    this->feed_nec_(v & 0x7FFF, (v & 0x8000) != 0);
    if (this->learn_mode_)
      this->sniff_(v & 0x7FFF, (v & 0x8000) != 0);
  }
  if (this->learn_mode_) {
    this->watch_rssi_();
    if (this->sniff_reps_ && millis() - this->sniff_last_ms_ > PRESS_GAP_MS) {
      ESP_LOGI(TAG, "LEARN raw   ...and %d more like it", this->sniff_reps_);
      this->sniff_reps_ = 0;
    }
  }
  if (this->txq_tail_ != this->txq_head_ && (int32_t) (millis() - this->tx_quiet_until_ms_) >= 0) {
    TxItem it = this->txq_[this->txq_tail_++ % TXQ];
    this->transmit_(it.address, it.command);
  }
}

void PacificFanRadio::feed_(uint16_t dur, bool mark) {
  switch (this->state_) {
    case WAIT_GAP:
      if (!mark && dur >= GAP_MIN) {
        this->bits_ = 0;
        this->frame_ = 0;
        this->state_ = EXPECT_MARK;
      }
      return;
    case EXPECT_MARK:
      if (!mark || dur < MARK_MIN || dur > MARK_MAX) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->mark_ = dur;
      this->state_ = EXPECT_SPACE;
      return;
    case EXPECT_SPACE:
      if (mark) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->frame_ = (this->frame_ << 1) | (this->mark_ > MARK_SPLIT ? 1 : 0);
      this->bits_++;
      if (dur >= GAP_MIN) {
        if (this->bits_ == 30)
          this->on_frame_(this->frame_);
        this->bits_ = 0;  // the next repeat starts right here
        this->frame_ = 0;
      } else if (this->bits_ > 30) {
        this->state_ = WAIT_GAP;
        return;
      }
      this->state_ = EXPECT_MARK;
      return;
  }
}

void PacificFanRadio::feed_nec_(uint16_t dur, bool mark) {
  switch (this->nec_state_) {
    case NEC_WAIT_GAP:
      if (!mark && dur >= GAP_MIN) {
        this->nec_bits_ = 0;
        this->nec_frame_ = 0;
        this->nec_state_ = NEC_EXPECT_MARK;
      }
      return;
    case NEC_EXPECT_MARK:
      if (!mark || dur < NEC_MARK_MIN || dur > NEC_MARK_MAX) {
        this->nec_state_ = NEC_WAIT_GAP;
        return;
      }
      this->nec_state_ = NEC_EXPECT_SPACE;
      return;
    case NEC_EXPECT_SPACE:
      if (mark) {
        this->nec_state_ = NEC_WAIT_GAP;
        return;
      }
      if (dur >= GAP_MIN) {  // the closing mark, then the gap
        if (this->nec_bits_ == 32)
          this->on_nec_frame_(this->nec_frame_);
        this->nec_bits_ = 0;  // the next repeat starts right here
        this->nec_frame_ = 0;
        this->nec_state_ = NEC_EXPECT_MARK;
        return;
      }
      if (dur < NEC_MARK_MIN || dur > NEC_SPACE_MAX || this->nec_bits_ == 32) {
        this->nec_state_ = NEC_WAIT_GAP;
        return;
      }
      this->nec_frame_ = (this->nec_frame_ << 1) | (dur > NEC_SPACE_SPLIT ? 1 : 0);
      this->nec_bits_++;
      this->nec_state_ = NEC_EXPECT_MARK;
      return;
  }
}

PacificRemote *PacificFanRadio::find_(uint32_t address) {
  for (auto *r : this->remotes_)
    if (r->address() == (address & 0xFFFFF))
      return r;
  return nullptr;
}

PacificRemote *PacificFanRadio::find_(Protocol protocol, uint32_t address) {
  for (auto *r : this->remotes_)
    if (r->protocol() == protocol && r->address() == address)
      return r;
  return nullptr;
}

void PacificFanRadio::on_frame_(uint32_t f) {
  uint32_t address = (f >> 10) & 0xFFFFF;
  uint16_t cmd = (f >> 1) & 0x1FF;
  bool total_even = !(__builtin_popcount(f) & 1);
  PacificRemote *remote = this->find_(PACIFIC, address);
  bool valid = remote != nullptr && total_even == remote->even_parity();
  this->on_code_(PACIFIC, f, address, cmd, remote, valid, total_even ? "parity: even" : "parity: odd");
}

void PacificFanRadio::on_nec_frame_(uint32_t f) {
  uint8_t cmd = (f >> 8) & 0xFF;
  if ((uint8_t) ~cmd != (f & 0xFF))
    return;  // corrupt: the last byte is the command inverted
  uint32_t address = f >> 16;
  PacificRemote *remote = this->find_(NEC, address);
  this->on_code_(NEC, f, address, cmd, remote, remote != nullptr, "protocol: nec");
}

void PacificFanRadio::on_code_(Protocol protocol, uint32_t f, uint32_t address, uint16_t cmd,
                               PacificRemote *remote, bool valid, const char *check) {
  uint32_t now = millis();
  const char *name = command_name(commands_of(protocol), cmd);
  char addr[12];
  snprintf(addr, sizeof(addr), protocol == NEC ? "0x%04X" : "0x%05X", (unsigned) address);

  if (this->learn_mode_) {
    this->heard_ms_ = now;
    if (now - this->learn_last_ms_ >= PRESS_GAP_MS)  // the first frame of a press
      this->log_lead_in_(protocol == NEC ? 65 : 59);
    bool repeat = f == this->learn_last_ && now - this->learn_last_ms_ < PRESS_GAP_MS;
    this->learn_last_ = f;
    this->learn_last_ms_ = now;
    if (!repeat) {
      ESP_LOGI(TAG, "LEARN address=%s %s command=0x%03X (%s)%s", addr, check, cmd, name,
               remote ? (valid ? "" : "  [known address, wrong parity]") : "  [unknown address]");
      // Shown in Home Assistant in the form the config wants it.
      if (this->last_heard_ != nullptr) {
        char buf[96];
        snprintf(buf, sizeof(buf), "address: %s, %s  (%s)", addr, check, name);
        this->last_heard_->publish_state(buf);
      }
    }
  }
  // Only valid frames take part in press detection: a corrupted frame, or
  // another fan's remote, must not reset a press that is still arriving.
  if (!valid)
    return;

  if (protocol == this->cand_protocol_ && f == this->cand_ && now - this->cand_last_ms_ < PRESS_GAP_MS) {
    this->cand_n_++;
  } else {
    this->cand_protocol_ = protocol;
    this->cand_ = f;
    this->cand_n_ = 1;
  }
  this->cand_last_ms_ = now;
  if (this->cand_n_ != 2)
    return;

  ESP_LOGI(TAG, "RX  %s  %s", addr, name);
  remote->on_rx(cmd);
}

// ============================================================
// Learn Mode for unknown remotes
// ============================================================
// The decoders only accept their own frame layouts and timings, so a remote
// with another encoding is dropped without a word.  Two probes that
// do not care about the encoding make it visible:
//  - the sniffer logs any train of clean pulses with its timings, and
//  - the RSSI watch logs any burst of RF energy on the tuned frequency, so
//    "wrong frequency" can be told apart from "right frequency, other code".

// Called on the gap that ends a frame of `frame_pulses` marks and spaces: logs
// the pulses before it, where a leader or preamble would be.
void PacificFanRadio::log_lead_in_(int frame_pulses) {
  const uint32_t before = 16;
  uint32_t end = this->hist_n_ - 1 - frame_pulses;  // first pulse of the frame
  if (this->hist_n_ < 1 + frame_pulses + before || frame_pulses + before >= HIST)
    return;
  char line[160];
  size_t p = 0;
  for (uint32_t i = end - before; i < end; i++)
    p += snprintf(line + p, sizeof(line) - p, "%s%d", i > end - before ? ", " : "", this->hist_[i & (HIST - 1)]);
  ESP_LOGI(TAG, "LEARN lead-in [%s]", line);
}

void PacificFanRadio::heard_(const char *what) {
  this->heard_ms_ = millis();
  if (this->last_heard_ != nullptr)
    this->last_heard_->publish_state(what);
}

void PacificFanRadio::sniff_(uint16_t dur, bool mark) {
  bool gap = !mark && dur >= SNIFF_GAP_US;
  if (dur >= SNIFF_MIN_US && !gap) {
    if (this->sniff_n_ == 0 && !mark)
      return;  // a packet starts with a mark
    this->sniff_buf_[this->sniff_n_++] = mark ? (int16_t) dur : -(int16_t) dur;
    if (this->sniff_n_ < SNIFF_MAX)
      return;
    gap = false;  // full: report what we have
  }
  if (this->sniff_n_ >= SNIFF_MIN_PULSES)
    this->sniff_report_(gap ? dur : 0);
  this->sniff_n_ = 0;
}

namespace {
struct Group {
  uint32_t sum{0};
  uint16_t n{0};
  uint16_t avg() const { return this->n ? this->sum / this->n : 0; }
};

// Groups durations of one sign that lie within 25% of each other, and
// returns how many fall into the two biggest groups.
int group_pulses(const int16_t *buf, int n, bool marks, Group *out, int &groups, int &total) {
  groups = 0;
  total = 0;
  for (int i = 0; i < n; i++) {
    if ((buf[i] > 0) != marks)
      continue;
    int d = std::abs(buf[i]);
    total++;
    int g = 0;
    while (g < groups && std::abs(d - out[g].avg()) * 4 > out[g].avg())
      g++;
    if (g == groups) {
      if (groups == 4)
        continue;
      groups++;
    }
    out[g].sum += d;
    out[g].n++;
  }
  std::sort(out, out + groups, [](const Group &a, const Group &b) { return a.n > b.n; });
  int top = groups > 0 ? out[0].n : 0;
  if (groups > 1)
    top += out[1].n;
  return top;
}

void describe(char *buf, size_t len, const Group *g, int groups) {
  size_t p = 0;
  buf[0] = 0;
  for (int i = 0; i < groups && p < len; i++)
    p += snprintf(buf + p, len - p, "%s%u us x%u", i ? ", " : "", g[i].avg(), g[i].n);
}
}  // namespace

void PacificFanRadio::sniff_report_(uint16_t gap) {
  uint32_t now = millis();
  // A frame a decoder understood is already reported as such.
  if (now - this->learn_last_ms_ < PRESS_GAP_MS)
    return;

  int n = this->sniff_n_;
  Group marks[4], spaces[4];
  int n_marks, n_spaces, total_marks, total_spaces;
  int top_marks = group_pulses(this->sniff_buf_, n, true, marks, n_marks, total_marks);
  int top_spaces = group_pulses(this->sniff_buf_, n, false, spaces, n_spaces, total_spaces);
  // Noise has no structure: a real code keeps its pulses to a few lengths.
  if (top_marks * 100 < total_marks * 85 || top_spaces * 100 < total_spaces * 85)
    return;

  // Two mark lengths = pulse-width coding (like Pacific); one mark length and
  // two space lengths = pulse-distance coding.
  const char *coding = nullptr;
  uint16_t split = 0;
  bool by_mark = false;
  auto two = [](const Group *g, int groups) { return groups >= 2 && g[1].n * 5 >= g[0].n; };
  if (two(marks, n_marks)) {
    coding = "pulse-width";
    by_mark = true;
    split = (marks[0].avg() + marks[1].avg()) / 2;
  } else if (two(spaces, n_spaces)) {
    coding = "pulse-distance";
    split = (spaces[0].avg() + spaces[1].avg()) / 2;
  }
  uint64_t bits = 0;
  int nbits = 0;
  if (coding != nullptr)
    for (int i = 0; i < n; i++)
      if ((this->sniff_buf_[i] > 0) == by_mark) {
        bits = (bits << 1) | (std::abs(this->sniff_buf_[i]) > split ? 1 : 0);
        nbits++;
      }

  // The same packet again within a press is a repeat: count it, don't print it.
  if (now - this->sniff_last_ms_ < PRESS_GAP_MS && std::abs(n - this->sniff_last_n_) <= 1 &&
      bits == this->sniff_last_bits_) {
    this->sniff_reps_++;
    this->sniff_last_ms_ = now;
    return;
  }
  if (this->sniff_reps_)
    ESP_LOGI(TAG, "LEARN raw   ...and %d more like it", this->sniff_reps_);
  this->sniff_reps_ = 0;
  this->sniff_last_n_ = n;
  this->sniff_last_bits_ = bits;
  this->sniff_last_ms_ = now;

  char m[96], s[96];
  describe(m, sizeof(m), marks, n_marks);
  describe(s, sizeof(s), spaces, n_spaces);
  int rssi = this->radio_->getRSSI();
  ESP_LOGI(TAG, "LEARN raw %d pulses%s, gap %u us, rssi %d dBm", n, n == SNIFF_MAX ? " (truncated)" : "", gap,
           rssi);
  ESP_LOGI(TAG, "LEARN raw   marks: %s", m);
  ESP_LOGI(TAG, "LEARN raw   spaces: %s", s);

  char hex[24] = "";
  if (coding != nullptr) {
    if (nbits > 32)
      snprintf(hex, sizeof(hex), "0x%X%08X", (unsigned) (bits >> 32), (unsigned) bits);
    else
      snprintf(hex, sizeof(hex), "0x%X", (unsigned) bits);
    ESP_LOGI(TAG, "LEARN raw   %s, %d bits = %s%s", coding, nbits, hex, nbits > 64 ? " (last 64)" : "");
  }
  // In the same form as ESPHome's remote_receiver raw dump.
  for (int i = 0; i < n; i += 16) {
    char line[160];
    size_t p = 0;
    for (int j = i; j < n && j < i + 16; j++)
      p += snprintf(line + p, sizeof(line) - p, "%s%d", j > i ? ", " : "", this->sniff_buf_[j]);
    ESP_LOGI(TAG, "LEARN raw   [%s]", line);
  }

  char summary[96];
  if (coding != nullptr)
    snprintf(summary, sizeof(summary), "unknown code: %s %d bits %s", coding, nbits, hex);
  else
    snprintf(summary, sizeof(summary), "unknown code: %d pulses, see log", n);
  this->heard_(summary);
}

void PacificFanRadio::watch_rssi_() {
  int rssi = this->radio_->getRSSI();
  uint32_t now = millis();
  if (this->rssi_floor_ == 0)
    this->rssi_floor_ = rssi;
  if (!this->rssi_active_) {
    if (rssi >= this->rssi_floor_ + RSSI_RISE_DB) {
      this->rssi_active_ = true;
      this->rssi_peak_ = rssi;
      this->rssi_start_ms_ = now;
      this->rssi_loud_ms_ = now;
    } else {
      this->rssi_floor_ += (rssi - this->rssi_floor_) / 32.0f;
    }
    return;
  }
  // OOK is silent between marks, so a single quiet sample ends nothing.
  this->rssi_peak_ = std::max(this->rssi_peak_, rssi);
  if (rssi >= this->rssi_floor_ + RSSI_RISE_DB / 2) {
    this->rssi_loud_ms_ = now;
    return;
  }
  if (now - this->rssi_loud_ms_ < RSSI_QUIET_MS)
    return;
  this->rssi_active_ = false;

  Tuning t = this->tuned_ < this->tunings_.size() ? this->tunings_[this->tuned_] : Tuning{FAN_MHZ, false};
  bool decoded = (int32_t) (this->heard_ms_ - this->rssi_start_ms_) >= 0;
  ESP_LOGI(TAG, "LEARN rf  %.3f MHz %s: %u ms of signal, peak %d dBm over a %d dBm floor%s", t.mhz,
           t.fsk ? "FSK" : "OOK", (unsigned) (this->rssi_loud_ms_ - this->rssi_start_ms_), this->rssi_peak_,
           (int) this->rssi_floor_, decoded ? "" : " - nothing decoded");
  if (!decoded) {
    char buf[96];
    snprintf(buf, sizeof(buf), "signal %d dBm at %.2f MHz %s, not decoded", this->rssi_peak_, t.mhz,
             t.fsk ? "FSK" : "OOK");
    this->heard_(buf);
  }
}

// ============================================================
// PacificRemote
// ============================================================
void PacificRemote::setup() {
  // Pacific keeps its original key, so existing fans keep their state.
  std::string key = this->protocol_ == NEC ? "pacific_fan_nec_" : "pacific_fan_";
  this->pref_ = global_preferences->make_preference<Phys>(fnv1_hash(key + to_string(this->address_)));
  Phys saved;
  this->restored_ = this->pref_.load(&saved);
  if (this->restored_) {
    this->phys_ = saved;
    if (this->phys_.speed > 6) this->phys_.speed = 1;
    if (this->phys_.speed != 0) this->last_speed_ = this->phys_.speed;
    if (this->phys_.level < 1 || this->phys_.level > this->dim_steps_) this->phys_.level = this->dim_steps_;
  }
  // The physical state is the truth: show it, whatever the entities restored.
  this->ready_ = true;
  this->publish_fan_();
  this->publish_light_();

  if (this->timer_sensor_ != nullptr)
    this->set_interval("timer_sensor", 30000, [this]() {
      if (this->timer_end_ms_ == 0) {
        this->timer_sensor_->publish_state(0);
        return;
      }
      int32_t left = (int32_t) (this->timer_end_ms_ - millis());
      this->timer_sensor_->publish_state(left > 0 ? (left + 59999) / 60000 : 0);
    });
}

void PacificRemote::dump_config() {
  if (this->protocol_ == NEC)
    ESP_LOGCONFIG(TAG, "NEC fan remote 0x%04X, %d dimmer steps", (unsigned) this->address_, this->dim_steps_);
  else
    ESP_LOGCONFIG(TAG, "Pacific fan remote 0x%05X (%s parity), %d dimmer steps", (unsigned) this->address_,
                  this->even_parity_ ? "even" : "odd", this->dim_steps_);
  ESP_LOGCONFIG(TAG, "  state %s: fan %s speed %d%s, light %s level %d", this->restored_ ? "restored" : "NOT restored",
                ONOFF(this->phys_.fan_on), this->phys_.speed, this->phys_.reverse ? " reverse" : "",
                ONOFF(this->phys_.light_on), this->phys_.level);
}

void PacificRemote::loop() {
  // The fan's own timer switches it off without a word on the air.
  if (this->timer_end_ms_ != 0 && (int32_t) (millis() - this->timer_end_ms_) >= 0) {
    this->timer_end_ms_ = 0;
    this->phys_.fan_on = false;
    this->save_();
    this->publish_fan_();
    if (this->timer_sensor_ != nullptr)
      this->timer_sensor_->publish_state(0);
  }
}

void PacificRemote::publish_fan_() {
  if (this->fan_ == nullptr)
    return;
  this->fan_->state = this->phys_.fan_on;
  this->fan_->speed = this->in_breeze() ? this->last_speed_ : this->phys_.speed;
  this->fan_->set_breeze(this->in_breeze());
  this->fan_->direction = this->phys_.reverse ? fan::FanDirection::REVERSE : fan::FanDirection::FORWARD;
  this->fan_->publish_state();
}

void PacificRemote::publish_light_() {
  if (this->light_ == nullptr)
    return;
  // Record it as what we want too, so the write it triggers sends nothing.
  this->want_light_on_ = this->phys_.light_on;
  this->want_brightness_ = (float) this->phys_.level / this->dim_steps_;
  auto call = this->light_->make_call();
  call.set_state(this->phys_.light_on);
  if (this->phys_.light_on)
    call.set_brightness(this->want_brightness_);
  call.set_transition_length(0);
  call.perform();
}

void PacificRemote::start_timer_(int hours) {
  this->timer_end_ms_ = millis() + (uint32_t) hours * 3600000UL;
  if (this->timer_sensor_ != nullptr)
    this->timer_sensor_->publish_state(hours * 60);
}

void PacificRemote::on_rx(uint16_t cmd) {
  const Commands &k = this->cmds_;
  int speed = speed_of(k, cmd);
  bool fan_changed = true;
  bool light_changed = true;

  if (speed) {
    this->phys_.fan_on = true;
    this->phys_.speed = speed;
    this->last_speed_ = speed;
  } else if (cmd == k.toggle) {
    this->phys_.fan_on = !this->phys_.fan_on;
  } else if (cmd == k.breeze) {
    this->phys_.fan_on = true;
    this->phys_.speed = 0;
  } else if (cmd == k.reverse) {
    this->phys_.reverse = !this->phys_.reverse;
  } else {
    fan_changed = false;
  }

  if (cmd == k.light) {
    this->phys_.light_on = !this->phys_.light_on;
  } else if (cmd == k.dim_up && this->phys_.light_on) {
    this->phys_.level = std::min<int>(this->dim_steps_, this->phys_.level + 1);
  } else if (cmd == k.dim_down && this->phys_.light_on) {
    this->phys_.level = std::max<int>(1, this->phys_.level - 1);
  } else {
    light_changed = false;
  }

  if (cmd == k.timer_1h)
    this->start_timer_(1);
  else if (cmd == k.timer_4h)
    this->start_timer_(4);
  else if (cmd == k.timer_8h)
    this->start_timer_(8);
  if (!this->phys_.fan_on)
    this->timer_end_ms_ = 0;

  this->save_();
  if (fan_changed)
    this->publish_fan_();
  if (light_changed)
    this->publish_light_();
}

void PacificRemote::press(Action action) {
  const Commands &k = this->cmds_;
  switch (action) {
    case TIMER_1H:
      this->send_(k.timer_1h);
      this->start_timer_(1);
      return;
    case TIMER_4H:
      this->send_(k.timer_4h);
      this->start_timer_(4);
      return;
    case TIMER_8H:
      if (k.timer_8h == NO_CMD)
        return;
      this->send_(k.timer_8h);
      this->start_timer_(8);
      return;
    case COLOUR:
      if (k.colour != NO_CMD) {
        this->send_(k.colour);
        return;
      }
      // No colour button: a quick off/on of the light moves to the next
      // colour, and leaves the light on.
      if (!this->phys_.light_on) {
        ESP_LOGW(TAG, "0x%05X: the light is off, not changing its colour", (unsigned) this->address_);
        return;
      }
      this->send_(k.light);
      this->send_(k.light);
      this->light_tx_ms_ = millis();
      return;
  }
}

void PacificRemote::fan_control(bool on, int speed, bool breeze, bool reverse) {
  if (!this->ready_)
    return;
  if (speed < 1 || speed > 6)
    speed = this->last_speed_;
  if (!on)
    this->timer_end_ms_ = 0;
  uint8_t target = breeze ? 0 : speed;

  if (!this->radio_->sync_only) {
    if (reverse != this->phys_.reverse)
      this->send_(this->cmds_.reverse);
    if (on && (!this->phys_.fan_on || target != this->phys_.speed)) {
      // Breeze and every speed command also switch the fan on, so they cover
      // "turn on" as well without relying on the toggle.
      this->send_(breeze ? this->cmds_.breeze : this->cmds_.speed[speed - 1]);
    } else if (!on && this->phys_.fan_on) {
      this->send_(this->cmds_.toggle);
    }
  }
  this->phys_.fan_on = on;
  this->phys_.speed = target;
  if (!breeze)
    this->last_speed_ = speed;
  this->phys_.reverse = reverse;
  this->save_();
  this->publish_fan_();
}

void PacificRemote::light_control(bool on, float brightness) {
  if (!this->ready_)
    return;
  this->want_light_on_ = on;
  this->want_brightness_ = brightness;
  // Keep light toggles apart: a later request replaces a pending one, so a
  // quick off-then-on collapses instead of flicking the colour.
  uint32_t since = millis() - this->light_tx_ms_;
  uint32_t wait = this->light_tx_ms_ && since < LIGHT_MIN_GAP_MS ? LIGHT_MIN_GAP_MS - since : 0;
  this->set_timeout("light", wait, [this]() { this->light_apply_(); });
}

void PacificRemote::light_apply_() {
  bool on = this->want_light_on_;
  int step = std::max(1, std::min(this->dim_steps_, (int) lroundf(this->want_brightness_ * this->dim_steps_)));

  if (this->radio_->sync_only) {
    this->phys_.light_on = on;
    if (on)
      this->phys_.level = step;
    this->save_();
    return;
  }
  if (on != this->phys_.light_on) {
    this->send_(this->cmds_.light);
    this->phys_.light_on = on;
    this->light_tx_ms_ = millis();
  }
  if (on && step != this->phys_.level) {
    int cur = this->phys_.level;
    // At either end, overshoot so the estimate re-anchors to reality.
    int n = step == this->dim_steps_ ? this->dim_steps_ - cur + DIM_OVERSHOOT
            : step == 1              ? cur - 1 + DIM_OVERSHOOT
                                     : std::abs(step - cur);
    uint16_t cmd = step > cur ? this->cmds_.dim_up : this->cmds_.dim_down;
    for (int i = 0; i < n; i++)
      this->send_(cmd);
    this->phys_.level = step;
  }
  this->save_();
}

// ============================================================
// Entities
// ============================================================
void PacificFan::control(const fan::FanCall &call) {
  bool on = call.get_state().value_or(this->state);
  int speed = call.get_speed().value_or(this->speed);
  auto dir = call.get_direction().value_or(this->direction);
  // Choosing a preset enters Breeze, choosing a speed leaves it, and a plain
  // "turn on" keeps whichever mode the fan was last in.
  bool breeze = this->remote_->in_breeze();
  if (call.has_preset_mode())
    breeze = strcmp(call.get_preset_mode(), PRESET_BREEZE) == 0;
  else if (call.get_speed().has_value())
    breeze = false;
  this->remote_->fan_control(on, speed, breeze, dir == fan::FanDirection::REVERSE);
}

light::LightTraits PacificLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
  return traits;
}

void PacificLight::write_state(light::LightState *state) {
  bool on = state->current_values.is_on();
  float brightness = state->current_values.get_brightness();
  this->remote_->light_control(on, brightness);
}

void PacificSwitch::write_state(bool state) {
  if (this->kind_ == LEARN_MODE)
    this->radio_->set_learn_mode(state);
  else
    this->radio_->sync_only = state;
  this->publish_state(state);
}

}  // namespace esphome::pacific_fan
