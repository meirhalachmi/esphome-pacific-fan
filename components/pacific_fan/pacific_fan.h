#pragma once

#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/button/button.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "cc1101_esp_arduino.h"

// ============================================================
// Ceiling fans over a CC1101 at 433.92 MHz OOK, two remote families:
//
// Pacific: 30 bits, MSB first = 20-bit address | 9-bit command | 1 check bit.
//   Each bit is one mark + one space: '1' = long mark + short space,
//   '0' = short mark + long space.  Frames repeat with a ~5.5ms gap.
//   The last bit makes the count of ones even on some remotes and odd on
//   others, so it is configured per remote and used to reject corrupt frames.
//
// NEC: a 9ms + 4.5ms leader, then 32 bits, MSB first = 16-bit address |
//   8-bit command | inverted command, then a closing mark.  Every mark is
//   short; a '1' is a long space, a '0' a short one.  Frames (each with its
//   leader) repeat with a ~20ms gap for ~1.2s.  The fan ignores frames
//   without the leader.
// ============================================================

namespace esphome::pacific_fan {

enum Protocol : uint8_t { PACIFIC = 0, NEC = 1 };

inline constexpr uint16_t NO_CMD = 0xFFFF;

// What each button of one family of remotes sends.
struct Commands {
  uint16_t toggle;  // power: toggles, no discrete off
  uint16_t breeze;
  uint16_t speed[6];
  uint16_t reverse;  // F/R or summer/winter: toggles
  uint16_t timer_1h, timer_4h, timer_8h;
  uint16_t light;   // toggles the light
  uint16_t colour;  // NO_CMD: a quick off/on of the light changes the colour
  uint16_t dim_down, dim_up;
};

// Measured on the original remotes.
inline constexpr Commands PACIFIC_COMMANDS = {
    0x191, 0x10B, {0x1E8, 0x1C8, 0x1A9, 0x189, 0x16A, 0x14A}, 0x12B, 0x095, 0x152, NO_CMD, 0x1B1, 0x1D0, 0x0F4, 0x133,
};
// WMT202-RS remote (רשתות תאורה 3NB60): no colour button.
inline constexpr Commands NEC_COMMANDS = {
    0x08, 0x40, {0x10, 0x90, 0x48, 0xC8, 0x88, 0x60}, 0xC0, 0x28, 0xA8, 0xFF, 0x98, NO_CMD, 0xA0, 0x20,
};

inline const Commands &commands_of(Protocol protocol) {
  return protocol == NEC ? NEC_COMMANDS : PACIFIC_COMMANDS;
}
const char *command_name(const Commands &cmds, uint16_t cmd);
int speed_of(const Commands &cmds, uint16_t cmd);  // 1..6 for a speed command, else 0

// What a button entity does; resolved to a command by the fan's family.
enum Action : uint8_t { TIMER_1H, TIMER_4H, TIMER_8H, COLOUR };

class PacificRemote;

// ============================================================
// The CC1101: always listening.  Between bursts it sends whatever press
// brings a fan closer to what Home Assistant asked for (see PacificRemote),
// or a press queued with send().
// ============================================================
class PacificFanRadio : public Component {
 public:
  void set_pins(int sck, int miso, int mosi, int cs, int gdo0, int gdo2) {
    sck_ = sck; miso_ = miso; mosi_ = mosi; cs_ = cs; gdo0_ = gdo0; gdo2_ = gdo2;
  }
  void add_remote(PacificRemote *remote) { remotes_.push_back(remote); }
  void set_last_heard(text_sensor::TextSensor *sensor) { last_heard_ = sensor; }
  // Where Learn Mode listens; index 0 must be the fans' own 433.92 MHz OOK.
  void add_tuning(float mhz, bool fsk) { tunings_.push_back({mhz, fsk}); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  bool is_ready() const { return ready_; }
  // Queue a press that is not part of a fan's tracked state (timers, the
  // colour button, the raw sender); sent one per loop so a batch cannot
  // starve the loop.
  void send(uint32_t address, uint16_t command);

  void set_learn_mode(bool on);
  void set_tuning(size_t index);
  bool learn_mode() const { return learn_mode_; }
  bool sync_only{false};
  // For measuring what a fan needs, without reflashing: frames per burst
  // (0 = the family's default) and the pause between two bursts of the same
  // command (-1 = the family's default).
  int repeats_override{0};
  int same_gap_override_ms{-1};

 protected:
  void pump_tx_();
  bool too_soon_(uint32_t address, uint16_t command);
  void transmit_(uint32_t address, uint16_t command);
  void transmit_pacific_(uint32_t address, uint16_t command, bool even_parity, int reps);
  void transmit_nec_(uint32_t address, uint16_t command, int reps);
  void tune_(size_t index);
  void start_rx_();
  void feed_(uint16_t duration, bool mark);
  void feed_nec_(uint16_t duration, bool mark);
  void on_frame_(uint32_t frame);
  void on_nec_frame_(uint32_t frame);
  // A frame that decoded cleanly: Learn Mode report, then press detection.
  void on_code_(Protocol protocol, uint32_t frame, uint32_t address, uint16_t cmd, PacificRemote *remote,
                bool valid, const char *check);
  PacificRemote *find_(uint32_t address);
  PacificRemote *find_(Protocol protocol, uint32_t address);
  void heard_(const char *what);
  void log_lead_in_(int frame_pulses);
  // Learn Mode, for remotes neither decoder understands.
  void sniff_(uint16_t duration, bool mark);
  void sniff_report_(uint16_t gap);
  void watch_rssi_();

  int sck_, miso_, mosi_, cs_, gdo0_, gdo2_;
  CC1101 *radio_{nullptr};
  bool ready_{false};
  std::vector<PacificRemote *> remotes_;
  text_sensor::TextSensor *last_heard_{nullptr};

  struct Tuning { float mhz; bool fsk; };
  std::vector<Tuning> tunings_;
  size_t tuning_{0};        // chosen for Learn Mode
  size_t tuned_{SIZE_MAX};  // what the CC1101 is set to now
  bool learn_mode_{false};

  struct TxItem { uint32_t address; uint16_t command; };
  static const uint8_t TXQ = 32;
  TxItem txq_[TXQ];
  uint8_t txq_head_{0}, txq_tail_{0};
  // The last burst: the same command again too soon reads as one long press.
  uint32_t last_tx_address_{0};
  uint16_t last_tx_command_{NO_CMD};
  uint32_t last_tx_ms_{0};
  size_t next_remote_{0};  // fans take turns

  uint32_t tail_{0};
  enum { WAIT_GAP, EXPECT_MARK, EXPECT_SPACE } state_{WAIT_GAP};
  uint16_t mark_{0};
  uint32_t frame_{0};
  int bits_{0};

  enum { NEC_WAIT_GAP, NEC_EXPECT_MARK, NEC_EXPECT_SPACE } nec_state_{NEC_WAIT_GAP};
  uint32_t nec_frame_{0};
  int nec_bits_{0};

  // A press is many identical frames (Pacific keys ~9 twice, NEC ~17 once).
  // A code counts once two valid frames agree, then its copies are
  // swallowed until it goes quiet.
  Protocol cand_protocol_{PACIFIC};
  uint32_t cand_{0};
  int cand_n_{0};
  uint32_t cand_last_ms_{0};
  uint32_t learn_last_{0};
  uint32_t learn_last_ms_{0};
  uint32_t heard_ms_{0};  // last time Learn Mode reported a decoded signal

  // Learn Mode: the latest pulses, to show what comes before a frame.
  static const uint32_t HIST = 128;  // power of two
  int16_t hist_[HIST];
  uint32_t hist_n_{0};

  // Learn Mode sniffer: any train of clean pulses, whatever its encoding.
  static const int SNIFF_MAX = 200;
  int16_t sniff_buf_[SNIFF_MAX];  // + mark / - space, in us
  int sniff_n_{0};
  int sniff_last_n_{0};
  uint64_t sniff_last_bits_{0};
  uint32_t sniff_last_ms_{0};
  int sniff_reps_{0};

  // Learn Mode RSSI watch: is anything transmitting on this frequency at all?
  float rssi_floor_{0};
  bool rssi_active_{false};
  int rssi_peak_{0};
  uint32_t rssi_start_ms_{0};
  uint32_t rssi_loud_ms_{0};
};

class PacificFan;

// ============================================================
// One remote = one fan + its light.  Keeps what the physical fan is
// believed to be doing, since power, F/R and the light are toggles, and what
// Home Assistant wants it to do.  The radio asks for one press at a time, so
// a newer request simply replaces whatever has not been sent yet.
// ============================================================
class PacificRemote : public Component {
 public:
  PacificRemote(PacificFanRadio *radio, Protocol protocol, uint32_t address, bool even_parity)
      : radio_(radio), protocol_(protocol), cmds_(commands_of(protocol)), address_(address),
        even_parity_(even_parity) {}

  void set_fan(PacificFan *fan) { fan_ = fan; }
  void set_light(light::LightState *light) { light_ = light; }
  void set_timer_sensor(sensor::Sensor *sensor) { timer_sensor_ = sensor; }
  void set_dim_steps(int steps) { dim_steps_ = steps; }
  void set_colour_guard(uint32_t ms) { colour_guard_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  Protocol protocol() const { return protocol_; }
  const Commands &commands() const { return cmds_; }
  uint32_t address() const { return address_; }
  bool even_parity() const { return even_parity_; }

  // A press heard from this remote.
  void on_rx(uint16_t cmd);
  // Home Assistant asked for this fan / light state.
  void fan_control(bool on, int speed, bool breeze, bool reverse);
  bool in_breeze() const { return want_.speed == 0; }
  void light_control(bool on, float brightness);
  // A button entity: send the command and track what it implies.
  void press(Action action);
  // The press that brings the fan closer to what is wanted, or NO_CMD.
  uint16_t next_press() const;
  // That press went out.
  void sent(uint16_t cmd);

 protected:
  struct Phys {
    bool fan_on;
    uint8_t speed;  // 1..6, or 0 = Breeze (a mode that replaces the speed)
    bool reverse;
    bool light_on;
    uint8_t level;
  } __attribute__((packed));

  void send_(uint16_t cmd) { radio_->send(address_, cmd); }
  // What a press does to the physical fan and light.
  void apply_(uint16_t cmd, bool &fan_changed, bool &light_changed);
  void start_timer_(int hours);
  void publish_fan_();
  void publish_light_();
  void save_() { pref_.save(&phys_); }

  PacificFanRadio *radio_;
  Protocol protocol_;
  const Commands &cmds_;
  uint32_t address_;
  bool even_parity_;
  PacificFan *fan_{nullptr};
  light::LightState *light_{nullptr};
  sensor::Sensor *timer_sensor_{nullptr};
  int dim_steps_{8};

  Phys phys_{false, 1, false, false, 8};
  Phys want_{false, 1, false, false, 8};
  ESPPreferenceObject pref_;
  bool ready_{false};
  bool restored_{false};

  int overshoot_{0};            // extra dimmer presses still owed at an end
  uint32_t colour_guard_ms_{3000};
  uint32_t light_off_ms_{0};    // when the light last went off; 0 = not since boot
  uint32_t timer_end_ms_{0};  // 0 = no timer running
  uint8_t last_speed_{1};     // shown while in Breeze, and restored on leaving it
};

// Breeze is a mode of its own on these fans: it replaces the current speed,
// and pressing a speed leaves it.  That is exactly a Home Assistant preset.
static const char *const PRESET_BREEZE = "Breeze";

class PacificFan : public Component, public fan::Fan {
 public:
  explicit PacificFan(PacificRemote *remote) : remote_(remote) { this->set_supported_preset_modes({PRESET_BREEZE}); }
  fan::FanTraits get_traits() override {
    fan::FanTraits traits(false, true, true, 6);
    this->wire_preset_modes_(traits);
    return traits;
  }
  void set_breeze(bool breeze) {
    if (breeze)
      this->set_preset_mode_(PRESET_BREEZE);
    else
      this->clear_preset_mode_();
  }

 protected:
  void control(const fan::FanCall &call) override;
  PacificRemote *remote_;
};

class PacificLight : public light::LightOutput {
 public:
  explicit PacificLight(PacificRemote *remote) : remote_(remote) {}
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

 protected:
  PacificRemote *remote_;
};

class PacificButton : public button::Button {
 public:
  PacificButton(PacificRemote *remote, Action action) : remote_(remote), action_(action) {}

 protected:
  void press_action() override { remote_->press(action_); }
  PacificRemote *remote_;
  Action action_;
};

class PacificTuningSelect : public select::Select, public Component {
 public:
  explicit PacificTuningSelect(PacificFanRadio *radio) : radio_(radio) {}
  void setup() override { this->publish_state((size_t) 0); }

 protected:
  void control(size_t index) override {
    this->radio_->set_tuning(index);
    this->publish_state(index);
  }
  PacificFanRadio *radio_;
};

class PacificSwitch : public switch_::Switch, public Component {
 public:
  enum Kind { LEARN_MODE, SYNC_ONLY };
  PacificSwitch(PacificFanRadio *radio, int kind) : radio_(radio), kind_(kind) {}
  void setup() override { this->publish_state(false); }

 protected:
  void write_state(bool state) override;
  PacificFanRadio *radio_;
  int kind_;
};

}  // namespace esphome::pacific_fan
