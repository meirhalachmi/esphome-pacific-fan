# esphome-pacific-fan

Control **Pacific ceiling fans** (433.92 MHz RF remotes) - and fans with
NEC-style 433.92 MHz remotes - from Home Assistant with an ESP32 and a CC1101. The fan, light, dimmer, direction and timers all
become Home Assistant entities, and the original remotes keep working: the
controller listens to them and keeps Home Assistant in sync.

Based on [sha1cybr/esp32-cc1101-rf-caprep](https://github.com/sha1cybr/esp32-cc1101-rf-caprep)
by Shai Dvash - see [Credits](#credits).

<p align="center">
  <img src="resources/controller.jpg" width="55%" alt="ESP32 with a CC1101 module and a 433 MHz antenna"/>
  &nbsp;
  <img src="resources/dashboard-room.jpg" width="28%" alt="One room in Home Assistant: light, fan and AC"/>
</p>

## Tested with

<img src="resources/remote.jpg" width="160" align="right" alt="The 15-button Pacific remote"/>

**Pacific Strong 42" CCT** DC ceiling fans with the LED light and the
15-button remote (sold in Israel by מחסני תאורה, SKU 75372015). Other Pacific
models with a similar remote are likely to work; if yours does not, see
[Other fans](#other-fans).

If your remote looks like this one - power and breeze on top, speeds 1-6
around F/R, 1H / 4H timers, light and colour, LED- / LED+ - it is the same
family.

<br clear="right"/>

<img src="resources/remote-wmt202-rs.jpg" width="130" align="right" alt="The 13-button WMT202-RS remote"/>

**רשתות תאורה 3NB60+LED** (60", 6 speeds, dimmable LED) with the
**WMT202-RS** remote, as `protocol: nec`. The remote has breeze and
summer/winter on top, power in the middle of speeds 1-6, 1H / 4H / 8H timers,
and LED- / light / LED+. Nothing is printed on its back: its address is fixed
inside the remote, and Learn Mode reads it like any other.

<br clear="right"/>

## What you get, per fan

| Entity | What it does |
|---|---|
| `fan` | On/off, 6 speeds, direction, and a **Breeze** preset |
| `light` | On/off and a brightness slider mapped onto the dimmer steps |
| `button` × 3 | Timer 1H, Timer 4H, Light colour (+ Timer 8H on `nec` remotes) |
| `sensor` | Minutes left on the fan's own timer |

Plus, for the whole controller: **Learn Mode**, **Last Heard** and **Learn
Frequency** (find a remote's address, or what an unknown remote sends), and
**Sync Only** (re-align the tracked state without transmitting).

## Hardware

- ESP32 dev board
- CC1101 module with a 433 MHz antenna

```
ESP32 GPIO  ->  CC1101
18          ->  SCK
19          ->  MISO (SO)
23          ->  MOSI (SI)
5           ->  CSN
25          ->  GDO0   (TX data)
26          ->  GDO2   (RX data)
3V3 / GND   ->  VCC / GND
```

Any pins can be used; set them in the config (below).

## Quick start

1. Copy [examples/fan-controller.yaml](examples/fan-controller.yaml) and create
   a `secrets.yaml` next to it with `wifi_ssid`, `wifi_password`, `api_key`
   (generate one with `openssl rand -base64 32`) and `ota_password`. Leave
   `fans:` empty (`fans: []`) if you don't know your addresses yet.
2. Flash it: `esphome run fan-controller.yaml`, and add the device in Home
   Assistant (Settings → Devices & Services → ESPHome).
3. **Find each remote's address**: on the device's page in Home Assistant,
   turn on **Learn Mode** and press any button on the remote. The **Last
   Heard** sensor shows exactly what to put in the config:
   ```
   address: 0xE5D7C, parity: even  (Power)
   address: 0x2882, protocol: nec  (Power)
   ```
   Repeat for each remote. (The same thing is in the device log as a `LEARN`
   line.) Turn Learn Mode off when done.
4. Add one entry per fan and flash again:
   ```yaml
   pacific_fan:
     fans:
       - name: Bedroom
         address: 0xE5D7C
         parity: even
       - name: Balcony
         protocol: nec
         address: 0x2882
   ```
5. **Calibrate once**: the controller cannot ask a fan what it is doing, so
   tell it. Turn on **Sync Only**, set the fan and light entities to match the
   room, turn Sync Only off.

The component is loaded straight from this repository:

```yaml
external_components:
  - source: github://meirhalachmi/esphome-pacific-fan
    components: [pacific_fan]
```

## Configuration

```yaml
pacific_fan:
  id: fan_radio          # optional, for id(fan_radio).send(address, command)
  sck_pin: 18            # all pins optional, defaults shown
  miso_pin: 19
  mosi_pin: 23
  cs_pin: 5
  gdo0_pin: 25
  gdo2_pin: 26
  learn_mode:            # optional, rename or hide the switches
    name: Learn Mode
  sync_only:
    name: Sync Only (no RF)
  last_heard:
    name: Last Heard
  learn_frequency:
    name: Learn Frequency
  fans:
    - name: Bedroom      # prefix for every entity of this fan
      protocol: pacific  # pacific (default) | nec
      address: 0xE5D7C   # remote address, from Learn Mode (20-bit; 16-bit for nec)
      parity: even       # even | odd, from Learn Mode (pacific only)
      dim_steps: 8       # optional, how many steps the dimmer has
      # Every entity can be customised, e.g.:
      # fan:   { name: Bedroom Ceiling Fan, icon: mdi:ceiling-fan }
      # light: { name: Bedroom Ceiling Light }
      # timer_1h / timer_4h / timer_8h (nec only) / colour / timer_remaining: { ... }
```

## Dashboard

[examples/dashboard-room-view.yaml](examples/dashboard-room-view.yaml) is a
ready-made Home Assistant view for one room (light, fan with quick actions,
optionally the AC) - the one in the screenshot above. Replace the entity prefix and paste it under `views:` in
the dashboard's raw configuration editor, once per room.

## How it works

**The protocol.** Each press sends a 30-bit PWM-OOK frame, most significant
bit first: 20-bit address, 9-bit command, 1 check bit. A `1` is a ~1090 µs mark
and a ~375 µs space, a `0` the reverse; frames repeat ~9 times with a ~5.6 ms
gap, and each press is keyed twice ~200 ms apart. The check bit makes the count
of ones even on some remotes and odd on others, hence `parity:` per fan.

| Button | Command | Button | Command |
|---|---|---|---|
| Power (toggle) | `0x191` | F/R (toggle) | `0x12B` |
| Breeze | `0x10B` | Timer 1H | `0x095` |
| Speed 1 | `0x1E8` | Timer 4H | `0x152` |
| Speed 2 | `0x1C8` | Light (toggle) | `0x1B1` |
| Speed 3 | `0x1A9` | Light colour | `0x1D0` |
| Speed 4 | `0x189` | LED- | `0x0F4` |
| Speed 5 | `0x16A` | LED+ | `0x133` |
| Speed 6 | `0x14A` | | |

**The NEC protocol** (`protocol: nec`, the WMT202-RS remote). A ~9 ms mark
and ~4.5 ms space leader, then 32 bits, most significant bit first: 16-bit
address, 8-bit command, then the command inverted, then a closing mark. Every
mark is ~560 µs; a `0` is followed by a ~560 µs space and a `1` by a ~1650 µs
one. Frames, each with its leader, repeat with a ~20 ms gap for ~1.2 s per
press. The fan ignores frames without the leader. The controller sends 10 frames and leaves 250 ms between
presses, so two presses are never read as one long one.

| Button | Command | Button | Command |
|---|---|---|---|
| Power (toggle) | `0x08` | Summer/winter (toggle) | `0xC0` |
| Breeze | `0x40` | Timer 1H | `0x28` |
| Speed 1 | `0x10` | Timer 4H | `0xA8` |
| Speed 2 | `0x90` | Timer 8H | `0xFF` |
| Speed 3 | `0x48` | Light (toggle) | `0x98` |
| Speed 4 | `0xC8` | LED- | `0xA0` |
| Speed 5 | `0x88` | LED+ | `0x20` |
| Speed 6 | `0x60` | | |

This remote has no colour button. The Light Colour entity sends a quick
off/on of the light instead, which is what changes the colour on these fans.

**Tracked state.** Power, F/R and the light are toggles and the dimmer only
steps, so the controller keeps what it believes each fan is doing (saved to
flash) and sends only what is needed to reach what Home Assistant asks for.
Turning a fan on is done with a speed command, which also switches it on, so
it never depends on guessing the toggle.

**Listening.** The CC1101 stays in receive mode, and every pulse goes to both
decoders; their timings do not overlap, so a mixed house of Pacific and NEC
remotes works side by side. A press counts once two identical frames agree,
with the right address and check bit (or inverted command); everything else
is dropped. Presses on the original remotes update the entities, and the
controller stops listening while it transmits so it never hears itself.

**Details worth knowing:**
- Breeze is a mode of its own on these fans: it replaces the speed, and
  pressing a speed leaves it. So it is a preset of the fan entity - choosing
  it enters Breeze, choosing a speed leaves it, and turning the fan back on
  returns to whichever of the two it was in.
- A quick off/on of the light changes its colour on these fans, so light
  toggles from Home Assistant are kept at least 3 s apart.
- The dimmer has no absolute command. Brightness maps to an estimated step;
  going to 100% or the minimum sends a couple of extra presses so the estimate
  re-anchors.
- The fan's own timer switches it off silently; the controller follows the
  countdown and marks the fan off when it ends (not across a controller reboot).

## Other fans

If Learn Mode shows nothing for a remote, it does not speak either protocol.
Learn Mode still reports it, in the device log (every line starts with
`LEARN`):

- `LEARN raw ...`: any train of clean pulses, with the mark and space lengths
  grouped, the bits decoded when the coding is pulse-width or pulse-distance,
  and the raw timings in the same form as ESPHome's `remote_receiver` dump.
  Last Heard shows `unknown code: ...`.
- `LEARN rf ...`: a burst of RF energy on the tuned frequency. `nothing
  decoded` means the frequency is right but the coding is not; no `LEARN rf`
  line at all means the remote transmits elsewhere. Try the other entries of
  **Learn Frequency** (433.42 / 434.42 / 418 / 315 / 310 / 303.875 MHz OOK,
  and 433.92 MHz FSK). Transmitting to the fans always goes back to
  433.92 MHz OOK.
- `LEARN lead-in [...]`: the pulses just before the first frame of a press,
  where a leader or preamble shows up (that is how the NEC leader was found).

The log is in the ESPHome dashboard, in `esphome logs`, or on the device's web
server: `curl -sN http://<device>/events | grep --line-buffered LEARN`.

For a full button-by-button capture,
[esphome/esphome_rf_capture.yaml](esphome/esphome_rf_capture.yaml) is a
receive-only build that logs every frame with its pulse timings; follow
[CAPTURE_PROCEDURE.md](esphome/CAPTURE_PROCEDURE.md) to map a remote.

## Troubleshooting

- **`CC1101 not detected`** in the log: check the SPI wiring and 3.3 V power.
- **Presses on the remote are not picked up**: turn on Learn Mode and check
  the frames arrive; move the antenna or the controller closer. If only
  `LEARN raw` / `LEARN rf` lines appear, see [Other fans](#other-fans).
- **Home Assistant shows the wrong state**: recalibrate with Sync Only.
- **WiFi fails**: the device opens a fallback access point if you add `ap:`
  under `wifi:` together with `captive_portal:`.

## Credits

This project is built on
[**sha1cybr/esp32-cc1101-rf-caprep**](https://github.com/sha1cybr/esp32-cc1101-rf-caprep)
by Shai Dvash and its contributors, which it was forked from. That repository did the groundwork
this one stands on: the ESP32 + CC1101 hardware setup, the RF capture/replay
tool, the first decoding of the Pacific fan protocol (the address + command
frame and the first command codes) and the first ESPHome version. What was
added here is the full button map measured from the remote, two-way state
tracking, and packaging as an ESPHome component.

The original general-purpose Arduino capture/replay tool (`server.ino`, with
its web UI and REST API) is not part of this repository any more; it lives on
in the upstream repository.

- CC1101 driver: [CC1101-ESP-Arduino](https://github.com/wladimir-computin/CC1101-ESP-Arduino)
  (MIT), vendored in `components/pacific_fan` because ESPHome's ESP-IDF build
  rejects its library manifest.
