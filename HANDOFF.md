# Handoff - ceiling fan RF controller

Three Pacific ceiling fans (rooms: **office**, **girls**, **ofek**) and one
רשתות תאורה 3NB60 with an NEC-style remote (**balcony**), all on 433.92 MHz
OOK, driven by one ESP32 + CC1101 through ESPHome.

## Status (2026-09-28)

The controller is now an ESPHome external component,
`components/pacific_fan` (C++ + Python); `esphome/esphome_fan_controller.yaml`
is this house's config (OTA at 192.168.1.202, device `fan-controller`), one
`fans:` entry per room.  README.md is the user-facing documentation and
`examples/` holds a minimal config and a dashboard view for others.

| Room | Address | Parity (total ones) |
|---|---|---|
| girls | 0xE5D7C | even |
| office | 0xAACAC | odd |
| ofek | 0xB19BC | even |
| balcony | 0x2882 | - (`protocol: nec`) |

Girls' room is verified end to end (TX moves the real fan and light, remote
presses update HA without being echoed).  Office and ofek were mapped with
Learn Mode; their command codes match the girls' remote.  A new room is one
more `fans:` entry.

## Measured (capture of the girls' remote, every button pressed twice)

- Address `0xE5D7C`.  **Parity: total ones even** for all ~45 frames.  The
  office remote uses odd, so parity is configured per fan, not globally.
- Timings: short mark 375us, long mark 1090us, inter-frame gap ~5.5ms, ~9
  frames per keying.  Every press keys twice, ~200ms apart.
- All 15 buttons are stateless (same code on every press):

  | Button | Cmd | Button | Cmd |
  |---|---|---|---|
  | Power (toggle) | 0x191 | F/R (toggle) | 0x12B |
  | Breeze | 0x10B | Timer 1H | 0x095 |
  | Speed 1-6 | 0x1E8 0x1C8 0x1A9 0x189 0x16A 0x14A | Timer 4H | 0x152 |
  | Light (toggle) | 0x1B1 | Light colour | 0x1D0 |
  | LED- | 0x0F4 | LED+ | 0x133 |

- The colour button is a real command, not a double light press.  A fast
  off/on of the light still changes colour, so HA should not do that.

## Design as built

- Physical state (a per-remote preference, flushed every 5s) is kept apart
  from the HA entities; HA changes are reconciled against it.  Moving to the
  component changed the preference keys, so state was reset once on
  2026-09-21 and needed a Sync Only recalibration.  Power and
  F/R are toggles, so off and direction changes are only sent when they differ.
  A speed command both sets speed and turns the fan on.
- RX: CC1101 always in RX, ISR on GDO2 into a ring buffer, decoded in a 20ms
  interval.  Frames that fail the address whitelist or the per-fan parity are
  dropped before press detection; a press counts once two identical valid
  frames agree.  RX is detached during TX.
- TX is queued and sent one burst per poll - back-to-back sends from the API
  handler previously starved the loop and tripped the task watchdog.
- **Sync Only (no RF)** switch: HA changes only re-align the physical state.
  Used to calibrate when the tracked state drifts.
- Learn Mode logs every frame, including unknown addresses.

## Balcony fan, NEC remote (2026-09-28)

The WMT202-RS remote has no printed address and Learn Mode showed nothing:
the Pacific decoder only accepts 30-bit Pacific frames and dropped it
silently.  Learn Mode gained a raw sniffer (`LEARN raw`: any clean pulse
train, grouped timings, pulse-width/-distance bits, raw dump), an RSSI watch
(`LEARN rf`: energy on the tuned frequency, "nothing decoded" if no decoder
took it) and a **Learn Frequency** select.  They showed 433.92 MHz OOK,
NEC-style: 16-bit address 0x2882, 8-bit command, inverted command, closing
mark; mark 560us, space 560/1650us, ~20ms gap, ~17 frames per press.  All
13 buttons were captured twice each and are stateless (table in README).
Fans now take `protocol: pacific|nec`; both decoders see every pulse.

Every frame starts with a 9ms + 4.5ms leader (NEC, as on infrared); the
sniffer missed it because the 4.5ms space split it off, and the fan ignores
frames without it.  Learn Mode now logs `LEARN lead-in`, the pulses before
the first frame of a press.  TX with the leader works (light toggled,
2026-09-28).  Range: the balcony is at the edge from the house centre
(~-78 dBm, one frame in many); the controller was moved to a spot that
reaches all four fans, and RX/TX were checked from there.

Open for the balcony fan:
- Does a speed press also switch the fan on (assumed, as on Pacific)?

Measured at the fan (2026-10-02): the dimmer takes 12 presses from minimum
to maximum either way, so `dim_steps: 13`.  An off/on of the light changes
the colour, and it does not have to be fast - how long the window is, is
not measured yet (HA keeps light toggles 3s apart).

## Built since

- **Dimmer slider**: the light is a monochromatic light; brightness maps to an
  estimated dimmer step (8 assumed, 7-8 observed). 100%/min overshoot by 2
  presses so the estimate re-anchors; remote LED+/- presses move the estimate.
- **Colour guard**: HA light toggles are kept >= 3s apart (restart-mode
  script), so a quick off/on from HA cannot change the colour.
- **Fan timers**: 1H/4H from HA or the remote start a countdown; the fan is
  marked off when it ends (not restored across a controller reboot).
  `<Room> Fan Timer` reports minutes left.
- **Security**: API encryption key and OTA password in `secrets.yaml`.
- HA: entities assigned to areas; "Comfort" dashboard (`/room-comfort`) with
  AC, fan, light and quick actions per room.

## Open

- Dimmer step count is still a guess (8).
- Unknown what cancels the fan's own timer. The countdown is cleared only
  when the fan goes off; if a speed press also cancels it on the fan, clear
  `timer_end_ms_` on speed commands too (`PacificRemote::on_rx` / `fan_control`).

## Toolchain notes

- ESPHome 2026.9 builds ESP32 with the ESP-IDF toolchain, which rejects the
  CC1101 library's manifest, so the library is vendored in
  `components/pacific_fan/` (the capture firmware includes it from there).
- WiFi credentials live in `esphome/secrets.yaml` (gitignored).

## Removed on purpose

`server.ino`, `homeassistant.yaml` and their images (the legacy Arduino
capture/replay tool) were deleted once the repo became `esphome-pacific-fan`;
they remain in git history and in the upstream repo
(sha1cybr/esp32-cc1101-rf-caprep), which the README credits.
