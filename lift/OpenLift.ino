/*********************************************************************
  OpenElevator — Elevator Simulator for Film Production
 **********************************************************************
  Platform : Teensy 4.1  (Arduino Mega 2560 compatible w/ minor pin
             changes — swap SW-SPI pins and verify EEPROM include)

  Architecture
  ─────────────────────────────────────────────────────────────────
  • EEPROM          — config persists across power cycles
  • SSD1306 OLED    — 128×64 SW-SPI (U8g2)
  • Rotary encoder  — navigation + long-press for config mode
  • 74HC165 chain   — up to 32 floor-call buttons (daisy-chainable)
  • 74HC595 chain   — matching button LEDs
  • Door relay pins — trigger prop/servo door controller

  Configurable (EEPROM)
  ─────────────────────────────────────────────────────────────────
  • numFloors           2 – MAX_FLOORS
  • floorLabels[]       up to LABEL_LEN chars each ("B2","G","M","1"…)
  • doorOpenDwell_ms    how long doors hold open
  • doorOpenTime_ms     door-open animation duration
  • doorCloseTime_ms    door-close animation duration
  • travelPerFloor_ms   simulated travel time per floor
  • dispatchMode        COLLECTIVE | NEAREST | MANUAL

  State Machine
  ─────────────────────────────────────────────────────────────────
  IDLE → DOORS_OPENING → DOORS_OPEN → DOORS_CLOSING → MOVING
       → ARRIVING → (back to DOORS_OPENING)
  CONFIG entered via 1.2-second encoder button hold.
 ***********************************************************************/

#include <Arduino.h>
#include <EEPROM.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#  include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#  include <Wire.h>
#endif

// ═══════════════════════════════════════════════════════════════════
//  PIN ASSIGNMENTS  — edit to match your board
// ═══════════════════════════════════════════════════════════════════

// OLED (SW-SPI, SSD1306 128×64) — matches Harry's VoV1366 wiring
constexpr uint8_t PIN_OLED_SCK  = 13;
constexpr uint8_t PIN_OLED_MOSI = 11;
constexpr uint8_t PIN_OLED_CS   = 34;
constexpr uint8_t PIN_OLED_DC   = 32;
constexpr uint8_t PIN_OLED_RST  = 33;

// Rotary encoder (any 3 GPIO)
constexpr uint8_t PIN_ENC_A   = 20;
constexpr uint8_t PIN_ENC_B   = 21;
constexpr uint8_t PIN_ENC_BTN = 22;   // push-to-click, active LOW

// 74HC165 shift-register chain — button inputs, active LOW
constexpr uint8_t PIN_SR_LOAD = 25;   // PL (parallel load, active LOW)
constexpr uint8_t PIN_SR_CLK  = 26;
constexpr uint8_t PIN_SR_DATA = 27;   // Q7 of last register in chain

// 74HC595 shift-register chain — button LEDs
constexpr uint8_t PIN_LED_LATCH = 28;
constexpr uint8_t PIN_LED_CLK   = 29;
constexpr uint8_t PIN_LED_DATA  = 30;

// Door relay outputs (active HIGH 120 ms pulse to trigger prop controller)
constexpr uint8_t PIN_DOOR_OPEN  = 35;
constexpr uint8_t PIN_DOOR_CLOSE = 36;

// ═══════════════════════════════════════════════════════════════════
//  COMPILE-TIME LIMITS
// ═══════════════════════════════════════════════════════════════════

constexpr uint8_t  MAX_FLOORS   = 32;   // hardware ceiling (4 × 74HC165)
constexpr uint8_t  NUM_SR_BYTES = 4;    // 4 registers × 8 bits = 32 buttons
constexpr uint8_t  LABEL_LEN    = 4;    // chars per floor label incl. '\0'
constexpr uint16_t EEPROM_MAGIC = 0xE1A7;
constexpr uint16_t EEPROM_BASE  = 0;

// ═══════════════════════════════════════════════════════════════════
//  ELEVATOR CONFIGURATION  (stored in EEPROM)
// ═══════════════════════════════════════════════════════════════════

enum DispatchMode : uint8_t {
  DISPATCH_COLLECTIVE = 0,   // serve all calls in travel direction first
  DISPATCH_NEAREST    = 1,   // always jump to closest pending call
  DISPATCH_MANUAL     = 2    // operator chooses next floor via encoder
};

struct ElevatorConfig {
  uint16_t     magic;
  uint8_t      numFloors;
  char         floorLabels[MAX_FLOORS][LABEL_LEN];
  uint16_t     doorOpenDwell_ms;
  uint16_t     doorOpenTime_ms;
  uint16_t     doorCloseTime_ms;
  uint16_t     travelPerFloor_ms;
  DispatchMode dispatchMode;

  void setDefaults() {
    magic             = EEPROM_MAGIC;
    numFloors         = 8;
    doorOpenDwell_ms  = 3000;
    doorOpenTime_ms   = 800;
    doorCloseTime_ms  = 800;
    travelPerFloor_ms = 1500;
    dispatchMode      = DISPATCH_COLLECTIVE;

    // Stock film-useful floor set: Basement, Ground, Mezzanine, 1–5
    const char* defaults[] = { "B", "G", "M", "1", "2", "3", "4", "5" };
    for (uint8_t i = 0; i < MAX_FLOORS; i++) {
      if (i < 8)  strncpy(floorLabels[i], defaults[i], LABEL_LEN);
      else        snprintf(floorLabels[i], LABEL_LEN, "%d", i - 2);
      floorLabels[i][LABEL_LEN - 1] = '\0';
    }
  }
};

ElevatorConfig cfg;

void loadConfig() {
  EEPROM.get(EEPROM_BASE, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    cfg.setDefaults();
    EEPROM.put(EEPROM_BASE, cfg);
  }
}

void saveConfig() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_BASE, cfg);
}

// ═══════════════════════════════════════════════════════════════════
//  ELEVATOR STATE MACHINE
// ═══════════════════════════════════════════════════════════════════

enum ElevatorState : uint8_t {
  EL_IDLE,
  EL_DOORS_OPENING,
  EL_DOORS_OPEN,
  EL_DOORS_CLOSING,
  EL_MOVING,
  EL_ARRIVING,
  EL_CONFIG
};

enum Direction : uint8_t {
  DIR_NONE = 0,
  DIR_UP   = 1,
  DIR_DOWN = 2
};

struct ElevatorRuntime {
  int8_t        currentFloor;
  int8_t        targetFloor;
  Direction     direction;
  ElevatorState state;
  float         doorPosition;         // 0.0 = closed, 1.0 = fully open
  unsigned long stateEnteredAt_ms;
  bool          callPending[MAX_FLOORS];   // destination calls (cab panel)
  bool          hallCallUp[MAX_FLOORS];    // hall call going up
  bool          hallCallDown[MAX_FLOORS];  // hall call going down
};

ElevatorRuntime el;

void initRuntime() {
  memset(&el, 0, sizeof(el));
  el.currentFloor      = 0;
  el.targetFloor       = 0;
  el.direction         = DIR_NONE;
  el.state             = EL_IDLE;
  el.doorPosition      = 0.0f;
  el.stateEnteredAt_ms = millis();
}

// ═══════════════════════════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════════════════════════

U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI u8g2(
  U8G2_R0,
  PIN_OLED_SCK, PIN_OLED_MOSI,
  PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST
);

// ── Runtime HUD ───────────────────────────────────────────────────
//
//  ┌──────────────────────────────────────────────────────────────┐
//  │ •           ▲            MOVE                                │
//  │ •         ┌───┐                                              │
//  │           │ G │          COLL                                │
//  │           └───┘                                              │
//  │  [██████          ██████]  ← door panels                     │
//  └──────────────────────────────────────────────────────────────┘
//
void drawHUD() {
  u8g2.clearBuffer();

  // Floor label — large, centred
  u8g2.setFont(u8g2_font_inb30_mn);
  const char* label = cfg.floorLabels[el.currentFloor];
  int16_t lw = u8g2.getStrWidth(label);
  u8g2.setCursor((128 - lw) / 2, 50);
  u8g2.print(label);

  // Direction arrow (top-centre)
  if (el.direction == DIR_UP) {
    u8g2.drawTriangle(59, 2, 69, 2, 64, 10);
  } else if (el.direction == DIR_DOWN) {
    u8g2.drawTriangle(59, 10, 69, 10, 64, 2);
  }

  // State badge (top-right, 4 chars)
  u8g2.setFont(u8g2_font_5x8_mr);
  u8g2.setCursor(92, 10);
  switch (el.state) {
    case EL_IDLE:          u8g2.print("IDLE"); break;
    case EL_DOORS_OPENING: u8g2.print("OPNG"); break;
    case EL_DOORS_OPEN:    u8g2.print("OPEN"); break;
    case EL_DOORS_CLOSING: u8g2.print("CLSG"); break;
    case EL_MOVING:        u8g2.print("MOVE"); break;
    case EL_ARRIVING:      u8g2.print("ARVG"); break;
    default: break;
  }

  // Dispatch mode badge (bottom-right)
  u8g2.setCursor(92, 62);
  switch (cfg.dispatchMode) {
    case DISPATCH_COLLECTIVE: u8g2.print("COLL"); break;
    case DISPATCH_NEAREST:    u8g2.print("NRST"); break;
    case DISPATCH_MANUAL:     u8g2.print("MANU"); break;
  }

  // Pending-call dots (left strip, bottom-up, up to 10 visible)
  uint8_t shown = min((uint8_t)10, cfg.numFloors);
  for (uint8_t i = 0; i < shown; i++) {
    if (el.callPending[i] || el.hallCallUp[i] || el.hallCallDown[i])
      u8g2.drawDisc(4, 54 - i * 5, 2, U8G2_DRAW_ALL);
  }

  // Door panels — two bars sliding from centre outward
  uint8_t halfOpen = (uint8_t)(el.doorPosition * 58.0f);
  u8g2.drawBox(0,          56, 64 - halfOpen, 8);   // left panel
  u8g2.drawBox(64 + halfOpen, 56, 64 - halfOpen, 8); // right panel

  u8g2.sendBuffer();
}

// ── Config menu ──────────────────────────────────────────────────

enum ConfigPage : uint8_t {
  CFG_MAIN = 0,
  CFG_FLOORS,
  CFG_LABELS,
  CFG_TIMING,
  CFG_DISPATCH,
  CFG_SAVE
};

ConfigPage cfgPage       = CFG_MAIN;
uint8_t    cfgCursor     = 0;
uint8_t    cfgLabelFloor = 0;
uint8_t    cfgLabelChar  = 0;

// Generic: highlight row i if it equals cursor; invert draw color
static void highlightRow(uint8_t i, uint8_t y, uint8_t h) {
  if (i == cfgCursor) {
    u8g2.drawBox(0, y, 128, h);
    u8g2.setDrawColor(0);
  }
}
static void resetColor() { u8g2.setDrawColor(1); }

void drawCfgMain() {
  static const char* items[] = {
    "Num Floors", "Floor Labels", "Timing", "Dispatch", "Save & Exit"
  };
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(2, 11, "CONFIG");
  u8g2.drawHLine(0, 13, 128);
  u8g2.setFont(u8g2_font_5x8_mr);
  for (uint8_t i = 0; i < 5; i++) {
    highlightRow(i, 15 + i * 10, 10);
    u8g2.drawStr(4, 23 + i * 10, items[i]);
    resetColor();
  }
  u8g2.sendBuffer();
}

void drawCfgFloors() {
  char buf[4];
  snprintf(buf, sizeof(buf), "%2d", cfg.numFloors);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(2, 11, "NUM FLOORS");
  u8g2.setFont(u8g2_font_inb30_mn);
  u8g2.drawStr(46, 52, buf);
  u8g2.setFont(u8g2_font_5x8_mr);
  u8g2.drawStr(0, 63, "Turn=+/-  Click=done");
  u8g2.sendBuffer();
}

// Character set available for floor labels
static const char CHARSET[] =
  " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr uint8_t CHARSET_LEN = sizeof(CHARSET) - 1;

static uint8_t charsetIdx(char c) {
  for (uint8_t i = 0; i < CHARSET_LEN; i++) if (CHARSET[i] == c) return i;
  return 0;
}

void drawCfgLabels() {
  char buf[24];
  const char* lbl = cfg.floorLabels[cfgLabelFloor];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(2, 11, "FLOOR LABEL");
  u8g2.setFont(u8g2_font_5x8_mr);
  snprintf(buf, sizeof(buf), "Floor %d/%d", cfgLabelFloor + 1, cfg.numFloors);
  u8g2.drawStr(2, 24, buf);
  // Show label with cursor under active char
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(20, 46, lbl);
  // cursor underline
  uint8_t cx = 20 + cfgLabelChar * 9;
  u8g2.drawBox(cx, 48, 9, 2);
  u8g2.setFont(u8g2_font_5x8_mr);
  u8g2.drawStr(0, 63, "Turn=char  Click=next");
  u8g2.sendBuffer();
}

void drawCfgTiming() {
  static const char* rows[]  = { "Dwell ms", "Open  ms", "Close ms", "Travel ms" };
  uint16_t*          vals[]  = {
    &cfg.doorOpenDwell_ms, &cfg.doorOpenTime_ms,
    &cfg.doorCloseTime_ms, &cfg.travelPerFloor_ms
  };
  char buf[22];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(2, 11, "TIMING");
  u8g2.setFont(u8g2_font_5x8_mr);
  for (uint8_t i = 0; i < 4; i++) {
    highlightRow(i, 13 + i * 12, 12);
    snprintf(buf, sizeof(buf), "%-9s %5u", rows[i], *vals[i]);
    u8g2.drawStr(2, 22 + i * 12, buf);
    resetColor();
  }
  u8g2.drawStr(0, 63, "Turn=+50ms  Click=next");
  u8g2.sendBuffer();
}

void drawCfgDispatch() {
  static const char* modes[] = {
    "Collective  (ECA)", "Nearest call", "Manual (encoder)"
  };
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(2, 11, "DISPATCH");
  u8g2.setFont(u8g2_font_5x8_mr);
  for (uint8_t i = 0; i < 3; i++) {
    bool active = ((uint8_t)cfg.dispatchMode == i);
    if (active) { u8g2.drawBox(0, 14 + i * 15, 128, 15); u8g2.setDrawColor(0); }
    u8g2.drawStr(4, 24 + i * 15, modes[i]);
    resetColor();
  }
  u8g2.drawStr(0, 63, "Turn=select  Click=done");
  u8g2.sendBuffer();
}

void drawCfgSave() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont15_tf);
  u8g2.drawStr(14, 35, "SAVING...");
  u8g2.sendBuffer();
  saveConfig();
  delay(600);
  u8g2.clearBuffer();
  u8g2.drawStr(22, 35, "SAVED.");
  u8g2.sendBuffer();
  delay(500);
}

void drawConfig() {
  switch (cfgPage) {
    case CFG_MAIN:     drawCfgMain();     break;
    case CFG_FLOORS:   drawCfgFloors();   break;
    case CFG_LABELS:   drawCfgLabels();   break;
    case CFG_TIMING:   drawCfgTiming();   break;
    case CFG_DISPATCH: drawCfgDispatch(); break;
    case CFG_SAVE:     break;   // handled immediately in input
  }
}

// ═══════════════════════════════════════════════════════════════════
//  ROTARY ENCODER  (polled, no ISR)
// ═══════════════════════════════════════════════════════════════════

namespace Encoder {
  volatile int8_t delta    = 0;
  volatile bool   pressed  = false;
  uint8_t         lastA    = HIGH;
  uint8_t         btnState = HIGH;
  unsigned long   lastDebounce = 0;

  void poll() {
    uint8_t a = digitalRead(PIN_ENC_A);
    uint8_t b = digitalRead(PIN_ENC_B);
    if (a != lastA) {
      if (a == LOW) delta += (b == HIGH) ? +1 : -1;
      lastA = a;
    }
    uint8_t btn = digitalRead(PIN_ENC_BTN);
    if (btn == LOW && btnState == HIGH && (millis() - lastDebounce) > 80) {
      pressed     = true;
      lastDebounce = millis();
    }
    btnState = btn;
  }

  int8_t consumeDelta()  { int8_t d = delta; delta = 0; return d; }
  bool   consumePress()  { if (pressed) { pressed = false; return true; } return false; }
  bool   isHeld()        { return digitalRead(PIN_ENC_BTN) == LOW; }
}

// ═══════════════════════════════════════════════════════════════════
//  SHIFT-REGISTER BUTTON SCAN  (74HC165, active LOW)
// ═══════════════════════════════════════════════════════════════════

namespace Buttons {
  uint8_t current[NUM_SR_BYTES] = {};
  uint8_t prev[NUM_SR_BYTES]    = {};
  uint8_t fell[NUM_SR_BYTES]    = {};  // 1 on rising edge (button pressed)

  void scan() {
    memcpy(prev, current, NUM_SR_BYTES);
    // Latch parallel inputs
    digitalWrite(PIN_SR_LOAD, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_SR_LOAD, HIGH);
    // Clock out all bytes
    for (uint8_t b = 0; b < NUM_SR_BYTES; b++) {
      current[b] = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {
        current[b] = (current[b] << 1) | digitalRead(PIN_SR_DATA);
        digitalWrite(PIN_SR_CLK, HIGH);
        delayMicroseconds(2);
        digitalWrite(PIN_SR_CLK, LOW);
      }
      current[b] ^= 0xFF;  // invert: active LOW → active HIGH
    }
    for (uint8_t b = 0; b < NUM_SR_BYTES; b++)
      fell[b] = current[b] & ~prev[b];
  }

  bool wasFell(uint8_t idx) {
    return (fell[idx / 8] >> (idx % 8)) & 1;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  LED SHIFT-OUT  (74HC595)
// ═══════════════════════════════════════════════════════════════════

namespace LEDs {
  uint8_t state[NUM_SR_BYTES] = {};

  void set(uint8_t idx, bool on) {
    if (on) state[idx / 8] |=  (1 << (idx % 8));
    else    state[idx / 8] &= ~(1 << (idx % 8));
  }

  void flush() {
    digitalWrite(PIN_LED_LATCH, LOW);
    for (int8_t b = NUM_SR_BYTES - 1; b >= 0; b--)
      shiftOut(PIN_LED_DATA, PIN_LED_CLK, MSBFIRST, state[b]);
    digitalWrite(PIN_LED_LATCH, HIGH);
  }

  void update() {
    for (uint8_t i = 0; i < cfg.numFloors; i++)
      set(i, el.callPending[i]);
    // Blink current-floor LED at 2 Hz
    set(el.currentFloor, (millis() / 250) & 1);
    flush();
  }
}

// ═══════════════════════════════════════════════════════════════════
//  DOOR OUTPUTS
// ═══════════════════════════════════════════════════════════════════

void pulseDoorOpen() {
  digitalWrite(PIN_DOOR_OPEN, HIGH);
  delay(120);
  digitalWrite(PIN_DOOR_OPEN, LOW);
}

void pulseDoorClose() {
  digitalWrite(PIN_DOOR_CLOSE, HIGH);
  delay(120);
  digitalWrite(PIN_DOOR_CLOSE, LOW);
}

// ═══════════════════════════════════════════════════════════════════
//  DISPATCH  — choose next target floor
// ═══════════════════════════════════════════════════════════════════

bool anyCallPending() {
  for (uint8_t i = 0; i < cfg.numFloors; i++)
    if (el.callPending[i] || el.hallCallUp[i] || el.hallCallDown[i]) return true;
  return false;
}

// Elevator Collective Algorithm: serve calls in direction of travel first
int8_t collectiveTarget() {
  // Calls ahead in current direction
  if (el.direction != DIR_DOWN) {
    for (int8_t i = el.currentFloor + 1; i < cfg.numFloors; i++)
      if (el.callPending[i] || el.hallCallUp[i]) return i;
  }
  if (el.direction != DIR_UP) {
    for (int8_t i = el.currentFloor - 1; i >= 0; i--)
      if (el.callPending[i] || el.hallCallDown[i]) return i;
  }
  // Reverse sweep
  for (int8_t i = el.currentFloor - 1; i >= 0; i--)
    if (el.callPending[i] || el.hallCallDown[i]) return i;
  for (int8_t i = el.currentFloor + 1; i < cfg.numFloors; i++)
    if (el.callPending[i] || el.hallCallUp[i]) return i;
  return -1;
}

int8_t nearestTarget() {
  int8_t best = -1, bestDist = 127;
  for (uint8_t i = 0; i < cfg.numFloors; i++) {
    if (!(el.callPending[i] || el.hallCallUp[i] || el.hallCallDown[i])) continue;
    if ((int8_t)i == el.currentFloor) continue;
    int8_t dist = abs((int8_t)i - el.currentFloor);
    if (dist < bestDist) { bestDist = dist; best = i; }
  }
  return best;
}

int8_t chooseTarget() {
  switch (cfg.dispatchMode) {
    case DISPATCH_COLLECTIVE: return collectiveTarget();
    case DISPATCH_NEAREST:    return nearestTarget();
    case DISPATCH_MANUAL:     return -1;   // operator-driven; set via encoder
    default:                  return -1;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  STATE MACHINE  TICK
// ═══════════════════════════════════════════════════════════════════

void enterState(ElevatorState next) {
  el.state             = next;
  el.stateEnteredAt_ms = millis();
}

void tickElevator() {
  unsigned long now     = millis();
  unsigned long elapsed = now - el.stateEnteredAt_ms;

  switch (el.state) {

    case EL_IDLE: {
      el.direction = DIR_NONE;
      int8_t tgt   = chooseTarget();
      if (tgt >= 0) {
        el.targetFloor = tgt;
        enterState(EL_DOORS_OPENING);
        pulseDoorOpen();
      }
      break;
    }

    case EL_DOORS_OPENING:
      el.doorPosition = constrain((float)elapsed / cfg.doorOpenTime_ms, 0.0f, 1.0f);
      if (elapsed >= cfg.doorOpenTime_ms) {
        el.doorPosition = 1.0f;
        enterState(EL_DOORS_OPEN);
      }
      break;

    case EL_DOORS_OPEN:
      el.doorPosition = 1.0f;
      // Clear calls served at this floor
      el.callPending[el.currentFloor]  = false;
      el.hallCallUp[el.currentFloor]   = false;
      el.hallCallDown[el.currentFloor] = false;
      if (elapsed >= cfg.doorOpenDwell_ms) {
        enterState(EL_DOORS_CLOSING);
        pulseDoorClose();
      }
      break;

    case EL_DOORS_CLOSING:
      el.doorPosition = constrain(
        1.0f - (float)elapsed / cfg.doorCloseTime_ms, 0.0f, 1.0f
      );
      if (elapsed >= cfg.doorCloseTime_ms) {
        el.doorPosition = 0.0f;
        if (el.currentFloor == el.targetFloor) {
          enterState(EL_IDLE);
        } else {
          el.direction = (el.targetFloor > el.currentFloor) ? DIR_UP : DIR_DOWN;
          enterState(EL_MOVING);
        }
      }
      break;

    case EL_MOVING: {
      el.doorPosition   = 0.0f;
      int8_t step       = (el.direction == DIR_UP) ? 1 : -1;
      int8_t nextFloor  = el.currentFloor + step;

      // Should we stop at the next floor?
      bool stopHere = (nextFloor == el.targetFloor);
      if (cfg.dispatchMode == DISPATCH_COLLECTIVE) {
        if (el.direction == DIR_UP   && el.callPending[nextFloor]) stopHere = true;
        if (el.direction == DIR_UP   && el.hallCallUp[nextFloor])  stopHere = true;
        if (el.direction == DIR_DOWN && el.callPending[nextFloor]) stopHere = true;
        if (el.direction == DIR_DOWN && el.hallCallDown[nextFloor]) stopHere = true;
      }

      if (elapsed >= cfg.travelPerFloor_ms) {
        el.currentFloor = nextFloor;
        if (stopHere) {
          enterState(EL_ARRIVING);
        } else {
          el.stateEnteredAt_ms = now;  // keep moving, reset timer
        }
      }
      break;
    }

    case EL_ARRIVING:
      // Re-evaluate target in case collective picked up a new call
      el.targetFloor = el.currentFloor;   // commit to this stop
      enterState(EL_DOORS_OPENING);
      pulseDoorOpen();
      break;

    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  CONFIG INPUT HANDLER
// ═══════════════════════════════════════════════════════════════════

void handleConfigInput() {
  int8_t delta = Encoder::consumeDelta();
  bool   click = Encoder::consumePress();

  switch (cfgPage) {

    case CFG_MAIN:
      cfgCursor = (cfgCursor + delta + 5) % 5;
      if (click) {
        switch (cfgCursor) {
          case 0: cfgPage = CFG_FLOORS;   cfgCursor = 0; break;
          case 1: cfgPage = CFG_LABELS;   cfgLabelFloor = 0; cfgLabelChar = 0; break;
          case 2: cfgPage = CFG_TIMING;   cfgCursor = 0; break;
          case 3: cfgPage = CFG_DISPATCH; break;
          case 4:
            drawCfgSave();
            cfgPage  = CFG_MAIN;
            el.state = EL_IDLE;
            break;
        }
      }
      break;

    case CFG_FLOORS:
      cfg.numFloors = (uint8_t)constrain((int)cfg.numFloors + delta, 2, MAX_FLOORS);
      if (click) cfgPage = CFG_MAIN;
      break;

    case CFG_LABELS: {
      char* lbl = cfg.floorLabels[cfgLabelFloor];
      if (delta != 0) {
        uint8_t ci = charsetIdx(lbl[cfgLabelChar]);
        ci         = (uint8_t)((ci + delta + CHARSET_LEN) % CHARSET_LEN);
        lbl[cfgLabelChar] = CHARSET[ci];
      }
      if (click) {
        cfgLabelChar++;
        if (cfgLabelChar >= LABEL_LEN - 1) {
          // Right-trim spaces, null-terminate
          for (int8_t c = LABEL_LEN - 2; c > 0 && lbl[c] == ' '; c--) lbl[c] = '\0';
          lbl[LABEL_LEN - 1] = '\0';
          cfgLabelChar = 0;
          cfgLabelFloor++;
          if (cfgLabelFloor >= cfg.numFloors) {
            cfgLabelFloor = 0;
            cfgPage = CFG_MAIN;
          }
        }
      }
      break;
    }

    case CFG_TIMING: {
      uint16_t* vals[] = {
        &cfg.doorOpenDwell_ms, &cfg.doorOpenTime_ms,
        &cfg.doorCloseTime_ms, &cfg.travelPerFloor_ms
      };
      *vals[cfgCursor] = (uint16_t)constrain((int)*vals[cfgCursor] + delta * 50, 100, 10000);
      if (click) {
        cfgCursor++;
        if (cfgCursor >= 4) { cfgCursor = 0; cfgPage = CFG_MAIN; }
      }
      break;
    }

    case CFG_DISPATCH:
      cfg.dispatchMode = (DispatchMode)(((uint8_t)cfg.dispatchMode + delta + 3) % 3);
      if (click) cfgPage = CFG_MAIN;
      break;

    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  RUNTIME INPUT HANDLER
// ═══════════════════════════════════════════════════════════════════

void handleRuntimeInput() {
  // Floor-call buttons
  for (uint8_t i = 0; i < cfg.numFloors; i++) {
    if (Buttons::wasFell(i)) {
      el.callPending[i] = true;
      if (el.state == EL_IDLE && el.currentFloor == (int8_t)i) {
        // Already here — just open the doors
        enterState(EL_DOORS_OPENING);
        pulseDoorOpen();
      }
    }
  }

  // Encoder delta: manual target selection when in MANUAL dispatch + IDLE
  int8_t delta = Encoder::consumeDelta();
  if (cfg.dispatchMode == DISPATCH_MANUAL && el.state == EL_IDLE && delta != 0) {
    el.targetFloor = (int8_t)constrain(el.targetFloor + delta, 0, cfg.numFloors - 1);
  }

  // Short click in MANUAL: confirm target
  if (Encoder::consumePress()) {
    if (cfg.dispatchMode == DISPATCH_MANUAL && el.state == EL_IDLE) {
      el.callPending[el.targetFloor] = true;
    }
  }

  // Long hold (≥1.2 s) → CONFIG MODE
  static unsigned long btnDownAt = 0;
  static bool          wasHeld   = false;
  bool held = Encoder::isHeld();
  if (held && !wasHeld)  btnDownAt = millis();
  if (held && (millis() - btnDownAt) > 1200 && !wasHeld) {
    cfgPage   = CFG_MAIN;
    cfgCursor = 0;
    el.state  = EL_CONFIG;
    btnDownAt = millis();   // prevent re-fire
  }
  wasHeld = held;
}

// ═══════════════════════════════════════════════════════════════════
//  SPLASH SCREEN
// ═══════════════════════════════════════════════════════════════════

void drawSplash() {
  u8g2.clearBuffer();
  u8g2.setFontMode(1);
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 52);
  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.setDrawColor(0);
  u8g2.drawStr(12, 16, "OpenElevator");
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x8_mr);
  u8g2.setDrawColor(0);
  u8g2.drawStr(8, 30, "Elevator Simulator");
  u8g2.drawStr(8, 42, "for Film Production");
  u8g2.setDrawColor(1);
  char buf[20];
  snprintf(buf, sizeof(buf), "%d floors loaded", cfg.numFloors);
  u8g2.drawStr(8, 62, buf);
  u8g2.sendBuffer();
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════

void setup() {
  // Encoder
  pinMode(PIN_ENC_A,   INPUT_PULLUP);
  pinMode(PIN_ENC_B,   INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);

  // Button shift-register inputs (74HC165)
  pinMode(PIN_SR_LOAD, OUTPUT); digitalWrite(PIN_SR_LOAD, HIGH);
  pinMode(PIN_SR_CLK,  OUTPUT); digitalWrite(PIN_SR_CLK,  LOW);
  pinMode(PIN_SR_DATA, INPUT_PULLUP);

  // LED shift-register outputs (74HC595)
  pinMode(PIN_LED_LATCH, OUTPUT); digitalWrite(PIN_LED_LATCH, LOW);
  pinMode(PIN_LED_CLK,   OUTPUT); digitalWrite(PIN_LED_CLK,   LOW);
  pinMode(PIN_LED_DATA,  OUTPUT);

  // Door relay outputs
  pinMode(PIN_DOOR_OPEN,  OUTPUT); digitalWrite(PIN_DOOR_OPEN,  LOW);
  pinMode(PIN_DOOR_CLOSE, OUTPUT); digitalWrite(PIN_DOOR_CLOSE, LOW);

  // Display
  u8g2.begin();
  u8g2.setFontMode(1);
  u8g2.setDrawColor(1);

  // Load config from EEPROM (or write defaults if blank)
  loadConfig();
  initRuntime();

  drawSplash();
  delay(1500);
}

void loop() {
  Encoder::poll();
  Buttons::scan();

  if (el.state == EL_CONFIG) {
    handleConfigInput();
    drawConfig();
  } else {
    handleRuntimeInput();
    tickElevator();
    drawHUD();
    LEDs::update();
  }
}
