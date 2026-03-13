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

The ceiling is 32 floors. `MAX_FLOORS` is a compile-time constant. `NUM_SR_BYTES` controls how many bytes are clocked out of the 74HC165 chain (one byte = one register = 8 floors). Add a 74HC165 and a 74HC595 to the chain, bump `NUM_SR_BYTES` by 1, recompile.

---

## Building

### With Nix (recommended)

The flake provides a dev shell with `arduino-cli`, `gnumake`, `picocom`, and `teensy-loader-cli` pinned via nixpkgs. All arduino-cli data directories are scoped to the project so this does not interfere with other Arduino work on the same machine.

```sh
nix develop
make install   # fetch the Teensy core and U8g2 from their respective indexes
make           # compile
make upload    # compile and flash (auto-detects /dev/ttyACM*)
make monitor   # open picocom at 115200
```

`make install` pulls the Teensy core from PJRC's package index and U8g2 from the Arduino library registry. It only needs to run once per machine, or after a `make clean`.

On Linux, uploading to a Teensy requires udev rules for the PJRC USB vendor ID. If `make upload` fails with a permissions error, install the rules file from https://www.pjrc.com/teensy/00-teensy.rules, copy it to `/etc/udev/rules.d/`, and run `udevadm control --reload-rules`.

If your port is not `/dev/ttyACM0`, override it:

```sh
make upload PORT=/dev/ttyACM1
```

### Without Nix

Install `arduino-cli` manually, then:

```sh
arduino-cli core update-index \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core install teensy:avr \
  --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli lib install "U8g2"
arduino-cli compile --profile open-elevator FilmLift.ino
arduino-cli upload --profile open-elevator --port /dev/ttyACM0 FilmLift.ino
```

Using the Arduino IDE: open `FilmLift.ino`, add the PJRC board manager URL under Preferences, install the Teensy core via Boards Manager, install U8g2 via Library Manager, select Teensy 4.1 under Tools > Board, upload.

### Library versions

`sketch.yaml` pins the libraries and platform version used during development. The current pins are:

```
Teensy core    teensy:avr 1.59.0
U8g2           2.35.19
```

Update these deliberately after testing. Do not let `make install` pull whatever is current without verifying the build.

---

## Directory structure

```
OpenElevator/
  FilmLift.ino       main sketch
  sketch.yaml        arduino-cli profile: board, core, library pins
  flake.nix          Nix dev shell
  Makefile           compile, upload, monitor, install targets
  README.md          this file
  LICENSE            Apache 2.0
  hardware/          (planned) KiCad schematics and PCB layout
  docs/              (planned) wiring diagrams and panel templates
```

---

## Contributing

Open issues and pull requests on GitHub. The project is in active development. Hardware design files, additional dispatch algorithms, multi-car support, and serial/OSC control are all on the table.