# OpenElevator

A configurable elevator simulator built on Teensy/Arduino hardware. Floor count, floor labels, door timing, travel speed, and dispatch behavior are all stored in EEPROM and editable at runtime through a rotary encoder and OLED menu. Buttons trigger calls. LEDs confirm them. Relay outputs fire a door controller. The whole thing runs as a state machine with no blocking delays in the main loop.

Originally built for film and television production. Equally useful for interactive installations, escape rooms, architectural mockups, theatrical props, or anything else that needs a convincing elevator panel.

---

## License

Apache License 2.0. See LICENSE.

---

## Hardware

### Controller

Teensy 4.1 is the reference platform. The code will run on a Mega 2560 with pin remapping and a different EEPROM include. Nothing in the design requires Teensy specifically.

### Required components

| Part | Notes |
|---|---|
| SSD1306 OLED 128x64 | SW-SPI wiring, U8g2 driver |
| Rotary encoder with push button | Any common EC11-type |
| 74HC165 shift register(s) | One per 8 floor buttons, daisy-chainable to 32 |
| 74HC595 shift register(s) | One per 8 button LEDs, matches 165 chain |
| Relay module or prop door controller | Two channels: open and close |

### Pin assignments

All pins are constants at the top of the file. Change them once, they propagate everywhere.

```
PIN_OLED_SCK   13
PIN_OLED_MOSI  11
PIN_OLED_CS    34
PIN_OLED_DC    32
PIN_OLED_RST   33

PIN_ENC_A      20
PIN_ENC_B      21
PIN_ENC_BTN    22

PIN_SR_LOAD    25   (74HC165 parallel load, active LOW)
PIN_SR_CLK     26
PIN_SR_DATA    27

PIN_LED_LATCH  28
PIN_LED_CLK    29
PIN_LED_DATA   30

PIN_DOOR_OPEN  35
PIN_DOOR_CLOSE 36
```

### Button wiring

Buttons wire to the parallel inputs of the 74HC165 chain, active LOW. The scan inverts the result so active HIGH is what the rest of the code sees. Wire floor call buttons starting at bit 0 of the first register. Add another 74HC165 in series to get the next 8 floors.

### LED wiring

LED anodes go to the 74HC595 outputs through appropriate current-limiting resistors. The LED index matches the button index. Button 0 and LED 0 are the same floor.

### Door outputs

Each door output goes HIGH for 120 ms on trigger. This is sized for a relay or opto-isolated input. If your prop controller wants a different pulse width, change the delay constant in `pulseDoorOpen()` and `pulseDoorClose()`.

---

## Configuration

Hold the encoder button for 1.2 seconds to enter config mode from anywhere in normal operation. Config is organized into five pages navigated by rotating the encoder and clicking to select or advance.

### Pages

**Num Floors**

Sets how many floors the system knows about, from 2 to 32. The number of buttons scanned and LEDs driven matches this value at runtime. Turn to change, click to return to the main config menu.

**Floor Labels**

Labels each floor one at a time, left to right through each floor in sequence. Each label holds up to three visible characters. Turn the encoder to cycle through the character set (space, hyphen, slash, 0-9, A-Z). Click advances to the next character position. After the last character position, the label is trimmed of trailing spaces, saved to the working config, and the cursor moves to the next floor. After the last floor, returns to the main config menu.

The character set is defined in `CHARSET[]` in the source. Add or remove characters there.

**Timing**

Four values, each adjustable in 50 ms steps by turning the encoder. Click moves to the next value. After the fourth, returns to the main config menu.

```
Door open dwell    how long the doors hold fully open before closing
Door open time     how long the opening animation takes
Door close time    how long the closing animation takes
Travel per floor   simulated travel time between adjacent floors
```

Range is 100 ms to 10000 ms per value.

**Dispatch**

Selects how the controller decides which floor to serve next. Turn to highlight, click to confirm and return to the main menu.

```
Collective    ECA algorithm. Serves all calls in the current direction
              of travel before reversing. This is how real traction
              elevators work.

Nearest       Always jumps to the closest pending call regardless of
              direction. Minimizes per-call latency at the cost of
              potentially bouncing constantly.

Manual        The encoder wheel selects the target floor and a short
              click confirms it. No automatic dispatch.
```

**Save and Exit**

Writes the working config to EEPROM and returns to normal operation. The EEPROM write only happens here. Exiting config by any other means discards changes.

### EEPROM layout

Config is a packed struct written at address 0 with a two-byte magic number sentinel (0xE1A7). If the sentinel does not match on boot, defaults are written to EEPROM and used. Default config is 8 floors labeled B, G, M, 1, 2, 3, 4, 5 with 3000 ms dwell, 800 ms open/close, 1500 ms per floor, collective dispatch.

---

## State machine

The elevator runs through six states. Transitions are time-driven. There are no `delay()` calls in the main loop so display updates and button scans run continuously.

```
IDLE
  No pending calls. Waits. On a new call, selects a target and
  moves to DOORS_OPENING.

DOORS_OPENING
  Fires the door open relay pulse once on entry. Animates
  doorPosition from 0.0 to 1.0 over doorOpenTime_ms.
  Moves to DOORS_OPEN.

DOORS_OPEN
  Holds doors fully open for doorOpenDwell_ms. Clears the call
  flag for the current floor. Moves to DOORS_CLOSING.

DOORS_CLOSING
  Fires the door close relay pulse once on entry. Animates
  doorPosition from 1.0 to 0.0 over doorCloseTime_ms.
  If already at the target floor, moves to IDLE.
  Otherwise moves to MOVING.

MOVING
  Advances one floor per travelPerFloor_ms in the direction of
  the current target. In Collective mode, checks for calls at
  the next floor that should be served in the current direction
  and stops early if needed. On arrival, moves to ARRIVING.

ARRIVING
  Commits to the current floor as the stop. Moves to
  DOORS_OPENING.
```

The CONFIG state is orthogonal. The main loop branches entirely into config handling when `el.state == EL_CONFIG`. Returning from config resets state to IDLE.

---

## Display

The HUD shows:

- Current floor label in a large font, centered
- Direction triangle at the top center (up or down, absent when idle)
- Four-character state badge, top right
- Four-character dispatch mode badge, bottom right
- Pending call indicators as filled circles on the left edge, one per floor, bottom-up, up to 10 visible
- Door panel animation across the bottom: two bars that slide outward from center as the doors open

The config UI uses the same display. Each page is a simple list or value editor with the selected item drawn inverted.

---

## Expanding floor count

Set `MAX_FLOORS` at the top of the sketch to however many floors you need. `NUM_SR_BYTES` is derived from it automatically as `(MAX_FLOORS + 7) / 8`, so you do not touch that constant. Add one 74HC165 and one 74HC595 to the shift register chains per additional 8 floors, recompile.

The EEPROM struct grows by `LABEL_LEN` (4) bytes per additional floor. Teensy 4.1 has 4284 bytes of emulated EEPROM. At 4 bytes per floor label plus the fixed fields, you run out of room somewhere past 200 floors. The practical ceiling is how many buttons fit on your panel.

---

## Building

### With Nix (recommended)

The flake uses [arduino-nix](https://github.com/bouk/arduino-nix) to fetch the Teensy core and U8g2 as Nix derivations and bake them into a wrapped `arduino-cli` binary. There are no runtime downloads and no dependency on arduino-cli's bundled Python.

```sh
nix develop
make           # compile
make upload    # compile and flash (auto-detects /dev/ttyACM*)
make monitor   # open picocom at 115200
```

On Linux, uploading to a Teensy requires udev rules for the PJRC USB vendor ID. If `make upload` fails with a permissions error, install the rules from https://www.pjrc.com/teensy/00-teensy.rules, copy to `/etc/udev/rules.d/`, and run `udevadm control --reload-rules`.

Override the port if needed:

```sh
make upload PORT=/dev/ttyACM1
```

### Pinned versions

Core and library versions are pinned in `flake.nix`:

```
Teensy core    teensy:avr 1.59.0
U8g2           2.35.19
```

The PJRC package index and Arduino library index are pinned as flake inputs and locked in `flake.lock`. To update them:

```sh
nix flake update teensy-index
nix flake update library-index
```

Then verify the build still works before committing the updated lock file.

To browse available versions from the nix repl:

```sh
nix repl
:lf .
pkgs.arduinoPackages.platforms.teensy.avr
pkgs.arduinoLibraries.U8g2
```

### Without Nix

Install `arduino-cli` manually, then fetch the Teensy core and U8g2:

```sh
arduino-cli core update-index \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core install teensy:avr \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli lib install "U8g2"
arduino-cli compile --fqbn teensy:avr:teensy41 FilmLift.ino
arduino-cli upload --fqbn teensy:avr:teensy41 --port /dev/ttyACM0 FilmLift.ino
```

Using the Arduino IDE: add the PJRC board manager URL under Preferences, install the Teensy core via Boards Manager, install U8g2 via Library Manager, select Teensy 4.1 under Tools > Board, upload.

---

## Directory structure

```
OpenElevator/
  FilmLift.ino       main sketch
  flake.nix          Nix dev shell, pinned Teensy core and U8g2 via arduino-nix
  flake.lock         locked input hashes
  Makefile           compile, upload, monitor targets
  README.md          this file
  LICENSE            Apache 2.0
  hardware/          (planned) KiCad schematics and PCB layout
  docs/              (planned) wiring diagrams and panel templates
```

---

## Contributing

Open issues and pull requests on GitHub. The project is in active development. Hardware design files, additional dispatch algorithms, multi-car support, and serial/OSC control are all on the table.