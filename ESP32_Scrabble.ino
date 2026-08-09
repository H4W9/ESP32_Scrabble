/* ============================================================================
   ESP32 Scrabble — Pancake (ESP32-C5, ST7796 320x480, FT6336 capacitive touch)
   ============================================================================
   German + English Scrabble. Standalone touch firmware sharing the H4W9 UI shell
   (header with back button + status corner, footer nav bar, momentum list menus,
   chip settings rows) with the ESP32_FlipSocial firmware, and sharing its SD
   dictionaries with the ESP32 Library reader.

   M1 (this file so far): shell, settings, WiFi, and a Dictionary screen that
   loads a .dwg word list into PSRAM and answers word lookups. Board, rules and
   the CPU opponent come at M2/M3 — see PLAN.md.

   Arduino IDE settings:
     Board            : ESP32C5 Dev Module
     Flash Size       : 8MB
     Partition Scheme : Custom  ->  partitions.csv in this folder
     Flash Frequency  : 80 MHz
     PSRAM            : Enabled     <-- required, the word lists live there

   Requires the patched TFT_eSPI-ESP32-C5 library with User_Setup_Select.h set to
   #include <User_Setup_marauder_pancake.h>.
   ============================================================================ */

#include "configs.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "ft6336.h"
#include "TouchKeyboard.h"
#include "theme.h"
#include "board_theme.h"
#include "vlw.h"
#include "dawg.h"
#include "game.h"
#include "letters.h"
#include "cpu.h"
#include "define.h"
#include "boardgrid.h"
#include "wordedit.h"

// Picoware core (panel init, touch, HTTP).
#include "src/Picoware/internal/boards.hpp"
#include "src/Picoware/internal/gui/draw.hpp"
#include "src/Picoware/internal/system/input.hpp"
#include "src/Picoware/internal/system/http.hpp"
#include "src/Picoware/internal/system/view.hpp"
#include "src/Picoware/internal/system/view_manager.hpp"
using namespace Picoware;

// Globals
#ifdef HAS_C5_SD
SPIClass sharedSPI(SPI);
#endif

static ViewManager *vm    = nullptr;   // owns Draw (panel) + InputManager (touch)
static TFT_eSPI    *tft   = nullptr;   // raw panel (from Draw) for the shell screens
static TouchInput  *touch = nullptr;   // FT6336 touch source (from InputManager)
static Theme        theme;             // colour theme + accent + font + brightness

// Theme-driven colours (macros so every use follows the current theme).
#define COL_BG     (theme.bg())
#define COL_FG     (theme.fg())
#define COL_ACCENT (theme.hdr())
#define COL_DIM    (theme.dim())
#define COL_SEL    (theme.sel())
static const uint16_t COL_OK = 0x07E0;   // status green (theme-independent)

// Panel size comes from the board block in configs.h. Rotation is left at the
// power-on default (0 = portrait), which suits the menus. The board view will
// switch to landscape at M2, where a 15x15 grid needs the wider axis.
static const int SCRW = TFT_WIDTH;
static const int SCRH = TFT_HEIGHT;

// Shell layout — matches H4W9 (header 28, nav 28, list rows 34).
static const int HDRH     = 28;
static const int NAVH     = 28;
// The settings list is drawn at fixed offsets and does not scroll, so the rows
// have to fit the panel: at 34 px they run off the bottom of the V8's 320 px
// screen. 26 px still clears the 22 px chips and the 16 px font.
#ifdef MARAUDER_V8
static const int ITEMH    = 26;
#else
static const int ITEMH    = 34;
#endif
static const int CONTENTY = HDRH;

// Game language. Picks which .dwg is loaded and, later, which tile distribution
// is used — the two must always agree, so they share this one setting.
enum Lang : uint8_t { LANG_DE = 0, LANG_EN = 1 };
static const char *const LANG_NAMES[2] = { "Deutsch", "English" };
static const char *const LANG_FILES[2] = { SCRABBLE_DIR "/de.dwg", SCRABBLE_DIR "/en.dwg" };
// Reduced "everyday vocabulary" lists. The CPU plays from one of these when the
// human-like setting is on, so it can't win with words nobody knows. Word
// validation always uses the FULL list, so the human is never restricted.
static const char *const LANG_COMMON[2] = { SCRABBLE_DIR "/de_common.dwg",
                                            SCRABBLE_DIR "/en_common.dwg" };

// Kept below the type declarations above: Arduino inserts its auto-generated
// prototypes ahead of the FIRST function in the sketch, so defining any function
// above these types would push the prototypes above them and break the build.
static Dawg    g_dawg;                 // full word list, used for validation (PSRAM)
static Dawg    g_dawgCpu;              // reduced list the CPU plays from (PSRAM)
static uint8_t g_lang = LANG_DE;       // persisted in /scrab_cfg.json
static bool    g_cpuCommon = true;     // "human-like vocabulary" for the CPU
static bool    g_luckHelper = true;    // balanced bag + steered draws (Game::refill)
static uint8_t g_crossLimit = MAX_CROSS_WORDS;   // parallel words a play may form

// The word list the move generator should search. Falls back to the full list
// if the reduced one is switched off or missing from the SD card.
static const Dawg &cpuDict() {
  return (g_cpuCommon && g_dawgCpu.loaded()) ? g_dawgCpu : g_dawg;
}

#ifndef HAS_CAP_TOUCH
// Resistive touch calibration (V8). Capacitive panels report real coordinates
// and need none of this. The 5 uint16 blob is TFT_eSPI's own format; it lives on
// SPIFFS next to the other settings.
static const char *TOUCH_CAL_FILE = "/scrab_touch.dat";

static bool touchCalLoad(uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "r");
  if (!f) return false;
  bool ok = (f.read((uint8_t *)cal, sizeof(uint16_t) * 5) == sizeof(uint16_t) * 5);
  f.close();
  return ok;
}
static void touchCalSave(const uint16_t *cal) {
  File f = SPIFFS.open(TOUCH_CAL_FILE, "w");
  if (!f) return;
  f.write((const uint8_t *)cal, sizeof(uint16_t) * 5);
  f.close();
}
// TFT_eSPI's 4-corner wizard. Blocking, and deliberately drawn without the theme
// so it is legible before anything else is up.
static void touchCalRun() {
  uint16_t cal[5];
  tft->fillScreen(TFT_BLACK);
  tft->setTextColor(TFT_WHITE, TFT_BLACK);
  tft->setTextDatum(MC_DATUM);
  tft->drawString("Touch Calibration", SCRW / 2, SCRH / 2 - 24, 4);
  tft->drawString("Tap each corner arrow", SCRW / 2, SCRH / 2 + 6, 2);
  tft->setTextDatum(TL_DATUM);
  delay(1500);
  tft->fillScreen(TFT_BLACK);
  tft->calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 15);
  tft->setTouch(cal);
  touchCalSave(cal);
}
static void touchCalInit() {
  uint16_t cal[5];
  if (touchCalLoad(cal)) tft->setTouch(cal);
  else                   touchCalRun();
}
#endif // !HAS_CAP_TOUCH

// Touch helpers
// Wait for a fresh tap (press edge) and return its point; blocks.
static bool waitTap(uint16_t &x, uint16_t &y, uint32_t timeoutMs = 0) {
  uint32_t start = millis();
  bool wasDown = touch->isPressed();
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) { x = touch->x(); y = touch->y(); return true; }
    wasDown = down;
    if (timeoutMs && (millis() - start) > timeoutMs) return false;
    delay(8);
    yield();
  }
}

static bool inRect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh) {
  return (int)x >= rx && (int)x < rx + rw && (int)y >= ry && (int)y < ry + rh;
}

// NOTE: the whole UI is portrait, so nothing rotates the panel. If a landscape
// view is ever added, rotate the panel AND the touch mapping together —
// TouchInput caches both the rotation it maps with and the bounds it clamps to
// (see TouchInput::setRotation), and rotating only the display leaves every tap
// mis-mapped and clamped to the portrait width. That bug made an earlier
// landscape board look frozen.

// Theme / brightness plumbing
static void applyBrightness() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, theme.duty());
#else
  ledcWrite(0, theme.duty());
#endif
}
static void applyThemeToViewManager() {
  if (!vm) return;
  vm->setBackgroundColor(theme.bg());
  vm->setForegroundColor(theme.fg());
  vm->setSelectedColor(theme.sel());
}

// Smooth-font drawing
// Every string in the UI goes through these. They keep the old TFT_eSPI font
// numbers (1/2/4) at the call sites but render with the VLW smooth fonts, which
// is what gives us real Ä Ö Ü ß glyphs — see vlw.h.
//
// Note TFT_eSPI ignores the font-number argument to drawString once a smooth
// font is loaded, so the correct VLW array MUST be loaded first; that is what
// these wrappers are for.
static void drawStr(const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(*tft, fontNum, g_vlw_cur_tft);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  tft->drawString(buf, x, y);
}
static int16_t strWidth(const String &s, uint8_t fontNum) {
  return vlwTextWidth(vlwData(fontNum), s.c_str());
}
// Variant taking an explicit sprite + font tracker: each sprite object carries
// its own loaded font, so it needs its own "currently loaded" tracker rather
// than sharing the panel's.
//
// NOTE this is deliberately NOT a template. Arduino generates function
// prototypes from the .ino with ctags and inserts them above everything else;
// for a template it emits the signature WITHOUT the `template<>` line, so the
// type parameter is undeclared and the sketch fails to compile. Templates in
// this project must live in a header (see vlwLoad in vlw.h).
static void sprStr(TFT_eSprite &g, const uint8_t *&track,
                   const String &s, int32_t x, int32_t y, uint8_t fontNum) {
  vlwLoad(g, fontNum, track);
  char buf[160];
  vlwPrivToUtf8(s.c_str(), buf, sizeof(buf));
  g.drawString(buf, x, y);
}

// Status LED. Colour-coded by action where the hardware allows:
//   amber = WiFi scan/connect, blue = SD/dictionary load, green = success, red = error.
#ifdef HAS_ACT_LED
// V8: one blue GPIO LED (active-high). GPIO28 is a strapping pin (pull-up =
// normal SPI boot), so it is a plain digital output — no PWM/LEDC on the strap
// pin and no pad-hold, so a reset always releases it and boots normally.
static bool g_actLedReady = false;
static void ledActArm() {
  if (g_actLedReady) return;
  pinMode(ACT_LED_PIN, OUTPUT);
  digitalWrite(ACT_LED_PIN, LOW);
  g_actLedReady = true;
}
static void ledActSet(bool on) {
  ledActArm();
  digitalWrite(ACT_LED_PIN, (on && theme.led_bright > 0) ? HIGH : LOW);
}
static inline void ledOff()  { ledActSet(false); }
static inline void ledWifi() { ledActSet(true); }
static inline void ledBusy() { ledActSet(true); }
static inline void ledOk()   { ledActSet(true); }
static inline void ledErr()  { ledActSet(true); }
static inline void ledBlinkOk(uint16_t ms = 150) { ledActSet(true); delay(ms); ledActSet(false); }
static void ledSet(bool on) { ledActSet(on); }

#else
// Pancake: onboard addressable RGB LED (WS2812-style).
#ifdef RGB_BUILTIN
  #define PW_RGB_PIN RGB_BUILTIN
#else
  #define PW_RGB_PIN LED_BUILTIN
#endif
// The RGB LED is WS2812-style: consecutive frames must be separated by a reset
// gap (>50us idle low) or the LED latches the first frame and passes the next
// one down the chain — so a colour followed immediately by off stays lit.
static inline void ledGap() { delayMicroseconds(300); }
static void ledRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t s = theme.led_bright;
  ledGap();
  rgbLedWrite(PW_RGB_PIN, (uint8_t)((uint16_t)r * s / 20),
                          (uint8_t)((uint16_t)g * s / 20),
                          (uint8_t)((uint16_t)b * s / 20));
}
static inline void ledOff()  { ledGap(); rgbLedWrite(PW_RGB_PIN, 0, 0, 0); }  // truly off
static inline void ledWifi() { ledRGB(255, 150, 0); }  // amber — scanning / connecting
static inline void ledBusy() { ledRGB(0,   80, 255); } // blue  — SD / dictionary work
static inline void ledOk()   { ledRGB(0,  255,   0); } // green — success
static inline void ledErr()  { ledRGB(255,  0,   0); } // red   — error
static inline void ledBlinkOk(uint16_t ms = 150) { ledOk(); delay(ms); ledOff(); }
static void ledSet(bool on) { if (on) ledWifi(); else ledOff(); }
#endif // HAS_ACT_LED

// Game config (SPIFFS: /scrab_cfg.json) — currently just the language.
static void cfgLoad() {
  File f = SPIFFS.open("/scrab_cfg.json", FILE_READ);
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return;
  uint8_t l = d["lang"].as<uint8_t>();
  if (l <= LANG_EN) g_lang = l;
  if (!d["cpucommon"].isNull()) g_cpuCommon = d["cpucommon"].as<bool>();
  if (!d["grid"].isNull()) {
    strncpy(g_gridName, d["grid"].as<const char *>(), GRID_NAME_MAX - 1);
    g_gridName[GRID_NAME_MAX - 1] = 0;
  }
  if (!d["luck"].isNull())      g_luckHelper = d["luck"].as<bool>();
  if (!d["cross"].isNull())     g_crossLimit = d["cross"].as<uint8_t>();
  if (g_crossLimit < 1 || g_crossLimit > 5) g_crossLimit = MAX_CROSS_WORDS;
}
static void cfgSave() {
  JsonDocument d;
  d["lang"] = g_lang;
  d["cpucommon"] = g_cpuCommon;
  d["luck"] = g_luckHelper;
  d["cross"] = g_crossLimit;
  d["grid"] = g_gridName;
  File w = SPIFFS.open("/scrab_cfg.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}

// Saved WiFi networks (SPIFFS: /scrab_wifi.json = {"nets":[{"s","p"}]})
static const int WIFI_MAX_SAVED = 12;
static int wifiLoad(String *ss, String *pp, int maxN) {
  File f = SPIFFS.open("/scrab_wifi.json", FILE_READ);
  if (!f) return 0;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return 0;
  JsonArray a = d["nets"].as<JsonArray>();
  if (a.isNull()) return 0;
  int n = 0;
  for (JsonVariant v : a) {
    if (n >= maxN) break;
    ss[n] = v["s"].as<String>();
    pp[n] = v["p"].as<String>();
    n++;
  }
  return n;
}
static void wifiWriteAll(String *ss, String *pp, int n) {
  JsonDocument d;
  JsonArray a = d["nets"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = ss[i];
    o["p"] = pp[i];
  }
  File w = SPIFFS.open("/scrab_wifi.json", FILE_WRITE);
  if (!w) return;
  serializeJson(d, w);
  w.close();
}
// Add/update a network, moving it to the front (most-recent-first).
static void wifiSave(const String &ssid, const String &pass) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  os[m] = ssid; op[m] = pass; m++;                 // new entry first
  for (int i = 0; i < n && m < WIFI_MAX_SAVED; i++) {
    if (ss[i] == ssid) continue;                   // drop old duplicate
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}
static String wifiPassFor(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  for (int i = 0; i < n; i++) if (ss[i] == ssid) return pp[i];
  return "";
}
static void wifiForget(const String &ssid) {
  String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
  int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
  String os[WIFI_MAX_SAVED], op[WIFI_MAX_SAVED];
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (ss[i] == ssid) continue;
    os[m] = ss[i]; op[m] = pp[i]; m++;
  }
  wifiWriteAll(os, op, m);
}

// Battery fuel gauge (MAX17048, I2C 0x36, shared bus)
// SOC register 0x04: high byte = integer %, low byte = 1/256 % (discarded).
static int      g_battPct = -1;      // -1 = unknown / gauge absent
static bool     g_battOk  = false;
static uint32_t g_battMs  = 0;
static void battInit() {
  Wire.beginTransmission(0x36);
  g_battOk = (Wire.endTransmission() == 0);
  Serial.println(g_battOk ? F("[Battery] MAX17048 OK") : F("[Battery] MAX17048 not found"));
}
static void battUpdate() {
  if (!g_battOk) return;
  Wire.beginTransmission(0x36);
  Wire.write(0x04);                          // SOC register
  if (Wire.endTransmission(false) != 0) { g_battOk = false; return; }
  Wire.requestFrom((uint8_t)0x36, (uint8_t)2);
  if (Wire.available() < 2) return;
  uint8_t hi = Wire.read();
  Wire.read();                               // fractional byte — discard
  g_battPct = (hi > 100) ? 100 : hi;
  g_battMs  = millis();
}

// True while a saved-network connect attempt is in flight (header icon = yellow).
static volatile bool g_wifiConnecting = false;

// Rendering helpers
// One 90°-wide WiFi arc (a real wifi-fan wedge: ±45° around straight up),
// plotted point-by-point so it doesn't depend on any drawArc angle convention.
static void wifiArc(int cx, int cy, int r, uint16_t c) {
  for (int deg = -45; deg <= 45; deg += 2) {
    float a = deg * 0.0174533f;
    int x = cx + (int)lroundf(r * sinf(a));
    int y = cy - (int)lroundf(r * cosf(a));
    tft->drawPixel(x, y, c);
  }
}

// Compact memory figures for the status bar, Marauder-style: "182k" / "3.9M".
// Deliberately terse — the header only has ~70 px to spare next to the title.
static String memShort(size_t bytes) {
  if (bytes >= 1024UL * 1024UL) {
    uint32_t tenths = (uint32_t)((bytes * 10ULL) / (1024ULL * 1024ULL));
    return String(tenths / 10) + "." + String(tenths % 10) + "M";
  }
  return String((uint32_t)(bytes / 1024)) + "k";
}

// DRAM + PSRAM free, always shown. Stacked on two lines so it costs ~46 px of
// width instead of ~110, and tucked just right of the back box when there is
// one. Drawn in the same colour as the battery %, as one status group.
static const int MEM_W = 46;
static int memX(bool showBack) { return showBack ? 46 : 6; }

static void drawHeaderMem(bool showBack) {
  int x = memX(showBack);
  tft->fillRect(x, 0, MEM_W, HDRH, COL_ACCENT);
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(ML_DATUM);
  drawStr(String("D:") + memShort(ESP.getFreeHeap()), x, HDRH / 2 - 6, 1);
  size_t ps = ESP.getFreePsram();
  if (ps) drawStr(String("P:") + memShort(ps), x, HDRH / 2 + 6, 1);
  tft->setTextDatum(TL_DATUM);
}

// Battery % (right edge) + WiFi state icon, painted into the header's top-right.
// Self-clearing, so it can also be called on its own for a periodic refresh.
static void drawHeaderStatus() {
  if (g_battOk && (g_battMs == 0 || millis() - g_battMs > 10000)) battUpdate();
  const int clearW = 62;
  tft->fillRect(SCRW - clearW, 0, clearW, HDRH, COL_ACCENT);   // clear the status corner

  int rx = SCRW - 4;                                   // right edge for battery text
  if (g_battPct >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_battPct);
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MR_DATUM);
    drawStr(pct, rx, HDRH / 2, 1);             // small (font 1) like H4W9
    rx -= strWidth(pct, 1) + 8;                  // slot the icon left of the %
  }

  // WiFi icon: source dot + three 90° arcs. green=connected, yellow=connecting, red=off.
  uint16_t wc = g_wifiConnecting               ? TFT_YELLOW
              : (WiFi.status() == WL_CONNECTED) ? COL_OK
                                                : TFT_RED;
  int cx = rx - 10, cy = HDRH / 2 + 5;                 // arc apex (bottom) point
  tft->fillCircle(cx, cy, 1, wc);
  wifiArc(cx, cy, 4,  wc);
  wifiArc(cx, cy, 7,  wc);
  wifiArc(cx, cy, 10, wc);

  tft->setTextDatum(TL_DATUM);
}

// Crisp vector chevron "<"/">" (solid triangle) — matches H4W9 selectors.
static void drawChevron(int bx, int by, int bw, int bh, bool right, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2;
  if (right) tft->fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, col);
  else       tft->fillTriangle(cx + 3, cy - 5, cx + 3, cy + 5, cx - 4, cy, col);
}
// Centered "+"/"-" (2px strokes) for the brightness selector.
static void drawPlusMinus(int bx, int by, int bw, int bh, bool plus, uint16_t col) {
  int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
  tft->fillRect(cx - r, cy - 1, 2 * r, 2, col);          // horizontal
  if (plus) tft->fillRect(cx - 1, cy - r, 2, 2 * r, col); // vertical
}

// H4W9-style header: optional back box with chevron (top-left), centred title,
// status corner (WiFi icon + battery %) top-right. With no back button the
// left slot carries the DRAM/PSRAM readout instead.
static void drawHeader(const String &title, bool showBack) {
  tft->fillRect(0, 0, SCRW, HDRH, COL_ACCENT);
  if (showBack) {
    tft->fillRoundRect(2, 3, 40, 22, 4, COL_ACCENT);
    tft->drawRoundRect(2, 3, 40, 22, 4, theme.neon(3, COL_DIM));
    drawChevron(2, 3, 40, 22, false, COL_FG);
  }
  tft->setTextColor(COL_FG, COL_ACCENT);
  tft->setTextDatum(MC_DATUM);
  // Centre the title in what is left between the memory readout and the status
  // corner, rather than on the screen, so it cannot sit under either.
  {
    int lo = memX(showBack) + MEM_W + 4, hi = SCRW - 62;
    drawStr(title, (lo + hi) / 2, HDRH / 2, 2);
  }
  drawHeaderMem(showBack);
  drawHeaderStatus();
  tft->setTextDatum(TL_DATUM);
}
static bool backTapped(uint16_t x, uint16_t y) {
  return (int)y < HDRH && (int)x < 48;   // top-left back box
}

// Footer nav bar (H4W9-style): up to three labelled rounded buttons in thirds.
static void drawNav(const char *l, const char *m, const char *r) {
  int y = SCRH - NAVH, third = SCRW / 3, bh = NAVH - 10, by = y + 5, bw = third - 10;
  tft->fillRect(0, y, SCRW, NAVH, COL_BG);
  tft->drawFastHLine(0, y, SCRW, theme.edge());
  const char *L[3] = { l, m, r };
  for (int i = 0; i < 3; i++) {
    if (!L[i] || !L[i][0]) continue;
    int cx = i * third + third / 2, bx = cx - bw / 2;
    tft->fillRoundRect(bx, by, bw, bh, 5, COL_ACCENT);
    tft->drawRoundRect(bx, by, bw, bh, 5, theme.neon(i, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    drawStr(L[i], cx, by + bh / 2, 2);
  }
  tft->setTextDatum(TL_DATUM);
}
// Which nav third was tapped: 0/1/2, or -1 if not in the footer band.
static int navHit(uint16_t x, uint16_t y) {
  if ((int)y < SCRH - NAVH) return -1;
  int c = (int)x / (SCRW / 3);
  return c > 2 ? 2 : c;
}

// One list row: fill, left text, optional right chevron, divider.
static void drawListRow(int y, const String &text, bool sel, bool arrow) {
  uint16_t bgc = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  tft->fillRect(0, y, SCRW, ITEMH, bgc);
  tft->setTextColor(COL_FG, bgc);
  tft->setTextDatum(ML_DATUM);
  drawStr(text, 12, y + ITEMH / 2, 2);
  if (arrow) drawChevron(SCRW - 26, y, 16, ITEMH, true, theme.neon(seed, COL_DIM));
  tft->drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  tft->setTextDatum(TL_DATUM);
}

// Sprite version of a list row (for flicker-free momentum scrolling).
// Which VLW array the current list sprite has loaded. A sprite is created and
// destroyed per scrollList() call and each carries its own font state, so this
// is reset right after createSprite() rather than persisting.
static const uint8_t *sprFont = nullptr;

static void drawRowSprite(TFT_eSprite &spr, int y, const String &text, bool arrow, int seed) {
  spr.fillRect(0, y, SCRW, ITEMH, COL_BG);
  spr.setTextColor(COL_FG, COL_BG);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, sprFont, text, 12, y + ITEMH / 2, 2);
  if (arrow) {
    int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
    spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  }
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

// Scrollbar drawn into a sprite: track + thumb at the right edge.
static void sprScrollBar(TFT_eSprite &spr, int viewH, int total, float scroll) {
  if (total <= viewH) return;
  const int bw = 4, bx = SCRW - bw - 1;
  spr.fillRect(bx, 0, bw, viewH, theme.edge());
  int thumbH = viewH * viewH / total; if (thumbH < 14) thumbH = 14;
  int maxS = total - viewH;
  int thumbY = (maxS > 0) ? (int)((scroll / (float)maxS) * (viewH - thumbH)) : 0;
  spr.fillRect(bx, thumbY, bw, thumbH, theme.neon(thumbY / 12, COL_DIM));
}

// scrollList return sentinels for footer-button taps (Back is SL_BACK).
static const int SL_BACK = -1, SL_F0 = -2, SL_F1 = -3, SL_F2 = -4;

// Momentum-scrolling list of string rows with a right-edge scrollbar. Optional
// footer nav bar (pass labels): a footer tap returns SL_F0/SL_F1/SL_F2, Back
// returns SL_BACK, and a row tap returns its index.
static int scrollList(const String &title, String *rows, int n, bool arrow,
                      const char *fL = nullptr, const char *fM = nullptr, const char *fR = nullptr) {
  bool hasFooter = (fL && fL[0]) || (fM && fM[0]) || (fR && fR[0]);
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY - (hasFooter ? NAVH : 0);
  int total = n * ITEMH;
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  if (hasFooter) drawNav(fL ? fL : "", fM ? fM : "", fR ? fR : "");

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  sprFont = nullptr;              // fresh sprite: nothing loaded on it yet

  float scroll = 0, fling = 0;
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (haveSpr) {
      spr.fillSprite(COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawRowSprite(spr, y, rows[i], arrow, i);
      }
      sprScrollBar(spr, CH, total, scroll);
      spr.pushSprite(0, CY);
    } else {
      tft->fillRect(0, CY, SCRW, CH, COL_BG);
      for (int i = 0; i < n; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawListRow(CY + y, rows[i], false, arrow);
      }
    }
  };
  render();

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();
    bool need = false;

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0; lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      scroll = pScroll + dy;
      uint32_t dt = now - lastT;
      if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
      need = true;
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); return SL_BACK; }
        if (hasFooter && (int)pY >= SCRH - NAVH) {          // footer button
          int nh = navHit(pX, pY);
          if (haveSpr) spr.deleteSprite();
          return nh == 0 ? SL_F0 : nh == 2 ? SL_F2 : SL_F1;
        }
        if ((int)pY >= CY && (int)pY < CY + CH) {
          int idx = ((int)pY - CY + (int)scroll) / ITEMH;
          if (idx >= 0 && idx < n) { if (haveSpr) spr.deleteSprite(); return idx; }
        }
      } else {
        fling = vel;
      }
      need = true;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      need = true;
    } else {
      fling = 0;
    }

    wasDown = down;
    if (need) render();
    delay(12);
  }
}

// Pick black or white text for a given fill, by luminance. The banner and the
// tile faces are theme-driven, so neither can assume a fixed text colour.
static uint16_t contrastOn(uint16_t c) {
  int r = ((c >> 11) & 0x1F) * 255 / 31;
  int g = ((c >> 5) & 0x3F) * 255 / 63;
  int b = (c & 0x1F) * 255 / 31;
  return ((r * 299 + g * 587 + b * 114) / 1000 > 140) ? TFT_BLACK : TFT_WHITE;
}

// Bottom status line (only on screens WITHOUT a footer nav bar).
static void statusLine(const char *msg, uint16_t col = 0xFFFF) {
  tft->fillRect(0, SCRH - 26, SCRW, 26, COL_BG);
  tft->setTextColor(col == 0xFFFF ? COL_FG : col, COL_BG);
  tft->setTextDatum(ML_DATUM);
  drawStr(msg, 8, SCRH - 13, 2);
  tft->setTextDatum(TL_DATUM);
}

// Settings chip rows: label + [<] value [>] (or [-] value [+])
static const int CHIP_W = 28, CHIP_H = 22;
// Geometry helper (draw + hit-test share it): fwd/bwd button x for a row value.
static void chipGeom(const String &val, int &fwd_bx, int &bwd_bx, int &vx) {
  fwd_bx = SCRW - 8 - CHIP_W;
  int vw = strWidth(val.c_str(), 2);
  vx     = fwd_bx - 4 - vw;
  bwd_bx = vx - 4 - CHIP_W;
}
static void drawChipRow(int y, const String &label, const String &val, bool pm,
                        bool sel, uint16_t valcol) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  drawListRow(y, label, sel, false);
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  int seed = y / ITEMH;
  tft->fillRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  tft->drawRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed, COL_DIM));
  tft->fillRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  tft->drawRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed + 4, COL_DIM));
  if (pm) {
    drawPlusMinus(bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    drawPlusMinus(fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  } else {
    drawChevron(bwd_bx, by, CHIP_W, CHIP_H, false, COL_FG);
    drawChevron(fwd_bx, by, CHIP_W, CHIP_H, true,  COL_FG);
  }
  tft->setTextColor(valcol ? valcol : COL_FG, rbg);
  tft->setTextDatum(ML_DATUM);
  drawStr(val, vx, y + ITEMH / 2, 2);
  tft->setTextDatum(TL_DATUM);
}
// Returns -1 none, 0 left/decrement, 1 right/increment. `val` must match draw.
static int chipHit(int y, const String &val, uint16_t x, uint16_t ty) {
  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  if ((int)ty < by || (int)ty >= by + CHIP_H) return -1;
  if ((int)x >= fwd_bx && (int)x < fwd_bx + CHIP_W) return 1;
  if ((int)x >= bwd_bx && (int)x < bwd_bx + CHIP_W) return 0;
  return -1;
}
// Label + right-aligned dim value + arrow (WiFi rows).
static void drawInfoRow(int y, const String &label, const String &val, bool sel) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  drawListRow(y, label, sel, true);
  if (val.length()) {
    tft->setTextColor(COL_DIM, rbg);
    tft->setTextDatum(MR_DATUM);
    drawStr(val, SCRW - 26, y + ITEMH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
}

// Sprite twins of the two settings row styles.
//
// The Settings list scrolls now, and scrolling has to composite: redrawing rows
// straight to the panel while a drag is in flight is exactly the flashing this
// firmware avoids everywhere else. TFT_eSprite's fillRect/drawChar are not
// virtual, so these are written out against TFT_eSprite rather than shared with
// the tft versions above — the same reason dstRow() in the Letters editor is.
static void sprChipRow(TFT_eSprite &spr, const uint8_t *&track, int y,
                       const String &label, const String &val, bool pm,
                       bool sel, uint16_t valcol) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  spr.fillRect(0, y, SCRW, ITEMH, rbg);
  spr.setTextColor(COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, label, 12, y + ITEMH / 2, 2);

  int by = y + (ITEMH - CHIP_H) / 2, fwd_bx, bwd_bx, vx;
  chipGeom(val, fwd_bx, bwd_bx, vx);
  spr.fillRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  spr.drawRoundRect(fwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed, COL_DIM));
  spr.fillRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, COL_ACCENT);
  spr.drawRoundRect(bwd_bx, by, CHIP_W, CHIP_H, 4, theme.neon(seed + 4, COL_DIM));

  int fcx = fwd_bx + CHIP_W / 2, bcx = bwd_bx + CHIP_W / 2, cy = by + CHIP_H / 2;
  if (pm) {
    const int r = 6;
    spr.fillRect(bcx - r, cy - 1, 2 * r, 2, COL_FG);
    spr.fillRect(fcx - r, cy - 1, 2 * r, 2, COL_FG);
    spr.fillRect(fcx - 1, cy - r, 2, 2 * r, COL_FG);
  } else {
    spr.fillTriangle(bcx + 3, cy - 5, bcx + 3, cy + 5, bcx - 4, cy, COL_FG);
    spr.fillTriangle(fcx - 3, cy - 5, fcx - 3, cy + 5, fcx + 4, cy, COL_FG);
  }

  spr.setTextColor(valcol ? valcol : COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, val, vx, y + ITEMH / 2, 2);
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

static void sprInfoRow(TFT_eSprite &spr, const uint8_t *&track, int y,
                       const String &label, const String &val, bool sel) {
  uint16_t rbg = sel ? COL_SEL : COL_BG;
  int seed = y / ITEMH;
  spr.fillRect(0, y, SCRW, ITEMH, rbg);
  spr.setTextColor(COL_FG, rbg);
  spr.setTextDatum(ML_DATUM);
  sprStr(spr, track, label, 12, y + ITEMH / 2, 2);
  if (val.length()) {
    spr.setTextColor(COL_DIM, rbg);
    spr.setTextDatum(MR_DATUM);
    sprStr(spr, track, val, SCRW - 32, y + ITEMH / 2, 2);
    spr.setTextDatum(ML_DATUM);
  }
  int cx = SCRW - 26 + 8, cy = y + ITEMH / 2;
  spr.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, theme.neon(seed, COL_DIM));
  spr.drawFastHLine(0, y + ITEMH - 1, SCRW, theme.neon(seed, theme.edge()));
  spr.setTextDatum(TL_DATUM);
}

// Centred message screen with a Back header: headline `a` + optional detail `b`
// (word-wrapped so long reasons stay readable). Blocks for a tap.
static void msgScreen(const char *title, const String &a, const String &b, uint16_t col) {
  tft->fillScreen(COL_BG);
  drawHeader(title, true);
  tft->setTextColor(col, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(a, SCRW / 2, SCRH / 2 - 20, 2);
  if (b.length()) {
    tft->setTextColor(COL_DIM, COL_BG);
    int y = SCRH / 2 + 6, maxW = SCRW - 24;
    String line = "", rest = b;
    while (rest.length() && y < SCRH - 20) {
      int sp = rest.indexOf(' ');
      String word = (sp < 0) ? rest : rest.substring(0, sp);
      String cand = line.length() ? line + " " + word : word;
      if (strWidth(cand.c_str(), 2) <= maxW) { line = cand; }
      else { drawStr(line, SCRW / 2, y, 2); y += 20; line = word; }
      rest = (sp < 0) ? "" : rest.substring(sp + 1);
    }
    if (line.length() && y < SCRH - 20) drawStr(line, SCRW / 2, y, 2);
  }
  tft->setTextDatum(TL_DATUM);
  uint16_t x, y2; waitTap(x, y2);
}

// WiFi
// Last STA disconnect reason (WIFI_REASON_*): 15 = 4-way handshake timeout
// (usually wrong password), 201 = no AP found (band/channel), 205 = conn fail.
static volatile int g_wifiReason = 0;
static volatile int g_wifiEvt = -1;   // last Arduino WiFi event id (-1 = none seen)
static bool g_manualDisconnect = false;   // user tapped Disconnect — don't auto-reconnect
static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  g_wifiEvt = (int)event;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    g_wifiReason = info.wifi_sta_disconnected.reason;
}

// Poll for association up to timeoutMs, animating a "connecting..." line at
// `spinnerY`. Tapping the screen cancels (returns false).
static bool waitConnect(uint32_t timeoutMs, int spinnerY) {
  uint32_t start = millis(), lastAnim = 0;
  bool wasDown = touch->isPressed();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    touch->run();
    bool down = touch->isPressed();
    if (down && !wasDown) return false;    // tap to cancel
    wasDown = down;
    if (millis() - lastAnim > 350) {
      lastAnim = millis();
      String d = "connecting";
      for (int i = 0; i < (dots = (dots + 1) % 4); i++) d += ".";
      tft->fillRect(0, spinnerY, SCRW, 20, COL_BG);
      tft->setTextColor(COL_DIM, COL_BG); tft->setTextDatum(MC_DATUM);
      drawStr(d, SCRW / 2, spinnerY + 8, 2);
      tft->setTextDatum(TL_DATUM);
    }
    delay(30);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Blocking connect with a clean loading screen. FlipperHTTP's ESP32-C5 approach:
// setBandMode(AUTO) + a plain WiFi.begin() — no scan / BSSID pin / radio cycle.
// setBandMode(AUTO) is THE C5-specific line: it lets the dual-band radio pick.
static bool connectWiFi(const String &ssid, const String &pass) {
  g_wifiConnecting = true;
  g_manualDisconnect = false;                // an explicit connect re-enables auto-reconnect
  ledWifi();
  g_wifiReason = 0;
  g_wifiEvt = -1;
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.scanDelete();                         // free any prior scan (harmless if none)
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);     // dual-band C5: auto-select the band
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(false);

  tft->fillScreen(COL_BG);
  drawHeader("WiFi", true);
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("Connecting to", SCRW / 2, SCRH / 2 - 22, 2);
  tft->setTextColor(COL_FG, COL_BG);
  drawStr(String("\"") + ssid + "\"", SCRW / 2, SCRH / 2 + 4, 4);
  tft->setTextDatum(TL_DATUM);

  bool ok = waitConnect(12000, SCRH / 2 + 34);
  g_wifiConnecting = false;
  ledOff();
  return ok;
}

static bool connectSaved(const String &ssid) {
  return connectWiFi(ssid, wifiPassFor(ssid));
}

// Background (re)connect — NON-BLOCKING so the menu stays responsive. An async
// scan orders the saved networks by signal strength (closest first); then it
// connects to each in turn. Driven from loop() via wifiBgTick().
enum WbState { WB_IDLE, WB_SCAN, WB_CONNECT, WB_DONE };
static WbState  g_wb  = WB_IDLE;
static uint32_t g_wbT = 0;
static String   g_wbSs[WIFI_MAX_SAVED], g_wbPp[WIFI_MAX_SAVED];
static int      g_wbN = 0, g_wbIdx = 0;

static void wifiBgTry() {                     // begin() on the current saved network
  g_wifiReason = 0; g_wifiEvt = -1;
  WiFi.begin(g_wbSs[g_wbIdx].c_str(), g_wbPp[g_wbIdx].c_str());
  WiFi.setAutoReconnect(false);
  g_wbT = millis();
}

static void wifiBgBegin() {
  g_wbN = wifiLoad(g_wbSs, g_wbPp, WIFI_MAX_SAVED);
  if (g_wbN == 0) { g_wb = WB_IDLE; return; }
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true);                    // async — order by RSSI when it completes
  g_wbT = millis();
  g_wb = WB_SCAN;
  g_wifiConnecting = true;
}

static void wifiBgTick() {
  if (g_wb == WB_SCAN) {
    int r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING && millis() - g_wbT < 6000) return;   // wait for the scan (<=6s)
    if (r > 0) {
      // Best RSSI of each saved net (−999 = out of range), then sort desc (closest first).
      int rssi[WIFI_MAX_SAVED];
      for (int i = 0; i < g_wbN; i++) {
        rssi[i] = -999;
        for (int j = 0; j < r; j++)
          if (WiFi.SSID(j) == g_wbSs[i] && WiFi.RSSI(j) > rssi[i]) rssi[i] = WiFi.RSSI(j);
      }
      for (int a = 0; a < g_wbN - 1; a++) {
        int best = a;
        for (int b = a + 1; b < g_wbN; b++) if (rssi[b] > rssi[best]) best = b;
        if (best != a) {
          int tr = rssi[a]; rssi[a] = rssi[best]; rssi[best] = tr;
          String ts = g_wbSs[a]; g_wbSs[a] = g_wbSs[best]; g_wbSs[best] = ts;
          String tp = g_wbPp[a]; g_wbPp[a] = g_wbPp[best]; g_wbPp[best] = tp;
        }
      }
    }
    WiFi.scanDelete();
    g_wbIdx = 0;
    wifiBgTry();
    g_wb = WB_CONNECT;
    return;
  }
  if (g_wb == WB_CONNECT) {
    if (WiFi.status() == WL_CONNECTED) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
    // Try only the two closest saved networks (once each, 8 s), then give up and
    // stay disconnected so the LED isn't lit the whole time we're offline.
    int maxTry = g_wbN < 2 ? g_wbN : 2;
    if (millis() - g_wbT > 8000) {
      if (++g_wbIdx >= maxTry) { g_wifiConnecting = false; g_wb = WB_DONE; return; }
      wifiBgTry();
    }
  }
}

// Scan / pick / password / connect flow. Smooth-scroll list, no paging.
static void scanFlow() {
  static String rows[41];
  for (;;) {
    tft->fillScreen(COL_BG);
    drawHeader("Scan", true);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr("Scanning...", SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
    ledSet(true);
    int nnet = WiFi.scanNetworks();
    ledSet(false);
    int rc = (nnet < 0) ? 0 : nnet;

    for (int i = 0; i < rc && i < 41; i++)
      rows[i] = WiFi.SSID(i) + "   ch" + WiFi.channel(i) + "  (" + WiFi.RSSI(i) + ")";

    int sel = scrollList("Scan", rows, rc, true, "Back", "Rescan", "");
    if (sel == SL_BACK || sel == SL_F0) return;        // Back
    if (sel == SL_F1) continue;                        // Rescan

    int idx = sel;
    if (idx < 0 || idx >= rc) continue;
    String ssid = WiFi.SSID(idx);
    char pass[65] = {0};
    String sp = wifiPassFor(ssid);
    if (sp.length()) strncpy(pass, sp.c_str(), sizeof(pass) - 1);
    if (!touchKeyboardInput(*tft, COL_FG, COL_BG, pass, sizeof(pass),
                            (String("Password: ") + ssid).c_str(), true)) continue;
    if (connectWiFi(ssid, pass)) {
      wifiSave(ssid, pass);
      statusLine("Connected!", COL_OK);
      uint16_t a, bb; waitTap(a, bb);
      return;
    }
    statusLine((String("Failed (reason ") + g_wifiReason + "). Tap to re-scan.").c_str(), TFT_RED);
    uint16_t a, bb; waitTap(a, bb);
  }
}

// WiFi Setup: saved networks (tap to connect) with a [Disconnect][Scan][Forget]
// footer. The header chevron is Back; the footer left button disconnects WiFi.
static void wifiSetup() {
  static String rows[WIFI_MAX_SAVED];
  for (;;) {
    String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
    int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
    for (int i = 0; i < n; i++) {
      bool cur = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ss[i]);
      rows[i] = (cur ? String("* ") : String("")) + ss[i];
    }
    int sel = scrollList("WiFi Setup", rows, n, true, "Disconnect", "Scan", n > 0 ? "Forget" : "");
    if (sel == SL_BACK) return;                            // header back
    if (sel == SL_F0) { WiFi.disconnect(true); g_manualDisconnect = true; continue; }
    if (sel == SL_F1) { scanFlow(); continue; }
    if (sel == SL_F2 && n > 0) {                           // Forget — pick a saved net
      static String frows[WIFI_MAX_SAVED];
      for (int i = 0; i < n; i++) frows[i] = ss[i];
      int f = scrollList("Forget", frows, n, true);
      if (f >= 0 && f < n) wifiForget(ss[f]);
      continue;
    }
    if (sel >= 0 && sel < n) connectSaved(ss[sel]);        // tap a saved network
  }
}

// WiFi Debug: live status/event/reason + heap, with reconnect actions.
static void wifiDebug() {
  // Repaint only when something actually changed. Repainting per tap made this
  // flash on any stray touch.
  bool repaint = true;
  for (;;) {
   if (repaint) {
    repaint = false;
    tft->fillScreen(COL_BG);
    drawHeader("WiFi Debug", true);
    int y = CONTENTY + 10;
    tft->setTextColor(COL_FG, COL_BG);
    tft->setTextDatum(TL_DATUM);
    auto line = [&](const String &s) { drawStr(s, 12, y, 2); y += 24; };
    bool up = (WiFi.status() == WL_CONNECTED);
    line(String("Status:       ") + WiFi.status() + (up ? "  (connected)" : ""));
    line(String("Last event:   ") + g_wifiEvt);
    line(String("Disc reason:  ") + g_wifiReason);
    line(String("SSID:         ") + (up ? WiFi.SSID() : String("-")));
    line(String("Channel:      ") + (up ? String(WiFi.channel()) : String("-")));
    line(String("IP:           ") + (up ? WiFi.localIP().toString() : String("-")));
    line(String("RSSI:         ") + (up ? String(WiFi.RSSI()) : String("-")));
    line(String("Free heap:    ") + ESP.getFreeHeap());
    line(String("Free PSRAM:   ") + ESP.getFreePsram());
    drawNav("Disconnect", "", "Reconnect");
   }

    uint16_t x, ty;
    if (!waitTap(x, ty)) continue;
    if (backTapped(x, ty)) return;                 // header back
    int nh = navHit(x, ty);
    if (nh == 0) { WiFi.disconnect(true); g_manualDisconnect = true; repaint = true; continue; }
    if (nh == 2) {
      repaint = true;
      String ss[WIFI_MAX_SAVED], pp[WIFI_MAX_SAVED];
      int n = wifiLoad(ss, pp, WIFI_MAX_SAVED);
      if (n) connectSaved(ss[0]);
    }
  }
}

// Word list
// Load the .dwg for the active language into PSRAM, with a progress screen —
// it is a ~2 MB SD read and takes a noticeable moment.
static bool dictLoad(bool showProgress) {
  if (showProgress) {
    tft->fillScreen(COL_BG);
    drawHeader("Dictionary", true);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setTextDatum(MC_DATUM);
    drawStr(String("Loading ") + LANG_NAMES[g_lang] + " word list...",
                    SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
  ledBusy();
  uint32_t t0 = millis();
  bool ok = g_dawg.load(LANG_FILES[g_lang]);
  uint32_t ms = millis() - t0;
  ledOff();
  if (ok) {
    Serial.printf("[Dict] %s: %lu words, %lu edges, %u KB in %lu ms\n",
                  LANG_FILES[g_lang], (unsigned long)g_dawg.wordCount(),
                  (unsigned long)g_dawg.edgeCount(),
                  (unsigned)(g_dawg.bytes() / 1024), (unsigned long)ms);
  } else {
    Serial.printf("[Dict] %s failed: %s\n", LANG_FILES[g_lang], g_dawg.error());
  }
  return ok;
}

// Convert typed text to the firmware's letter bytes: upper-case, and fold the
// UTF-8 umlauts the keyboard can produce into the private codes the word lists
// use (0x80 Ä, 0x82 Ö, 0x84 Ü). Case is folded because tiles are upper-case
// only, so ä and Ä are the same tile.
//
// ß EXPANDS TO SS, matching generate_scrabble_dict.py: there is no eszett tile
// in the German set, so a word containing one is played as two S. Typing ß and
// having it silently fail to match would be the confusing alternative.
static uint8_t normaliseWord(const char *in, uint8_t *out, uint8_t maxLen) {
  uint8_t n = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && n < maxLen; p++) {
    uint8_t c = *p;
    if (c == 0xC3 && p[1]) {                    // 2-byte UTF-8 Latin-1 supplement
      uint8_t d = *++p;
      switch (d) {
        case 0x84: case 0xA4: out[n++] = 0x80; break;   // A/a umlaut
        case 0x96: case 0xB6: out[n++] = 0x82; break;   // O/o umlaut
        case 0x9C: case 0xBC: out[n++] = 0x84; break;   // U/u umlaut
        case 0x9F:                                      // eszett -> SS
          out[n++] = 'S';
          if (n < maxLen) out[n++] = 'S';
          break;
        default: return 0;                              // not a playable letter
      }
    } else if (c >= 'a' && c <= 'z') {
      out[n++] = c - 'a' + 'A';
    } else if (c >= 'A' && c <= 'Z') {
      out[n++] = c;
    } else if (c == 0x80 || c == 0x82 || c == 0x84) {
      out[n++] = c;                              // Ä Ö Ü straight from the keyboard
    } else if (c == 0x81 || c == 0x83 || c == 0x85) {
      out[n++] = c - 1;                          // ä ö ü -> upper-case tile
    } else if (c == 0x86) {                      // ß -> SS, as above
      out[n++] = 'S';
      if (n < maxLen) out[n++] = 'S';
    } else {
      return 0;                                  // space, digit, punctuation
    }
  }
  return n;
}

// Letter bytes back to a drawable string. With the VLW fonts this is a straight
// copy — the private codes ARE the glyphs, so no transliteration is needed.
static String wordDisplay(const uint8_t *w, uint8_t len) {
  String s;
  for (uint8_t i = 0; i < len; i++) s += (char)w[i];
  return s;
}

// Board Builder — the premium-square layout, editable.
//
// A grid of cells above a tray of bonus tiles. Pick a bonus from the tray, then
// tap squares to lay it down; tapping a square that already carries the selected
// bonus takes it back off, and the Erase brush clears whatever is there.
//
// A tap paints the cell's whole SYMMETRY ORBIT — both mirrors and the transpose.
// An asymmetric board gives one
// axis more scoring potential than the other, and because the opponent searches
// both axes evenly, that would surface as the CPU looking biased for a reason
// that is really the board's. See boardgrid.h.
//
// The counts line is the premium budget. Classic spends DL 24 / TL 20 / DW 16 /
// TW 8 plus the star; staying near that keeps a custom board scoring like the
// real one rather than turning into a points fountain.
static const int BB_TRAYH = (SCRH > 400) ? 40 : 26;
static const int BB_TRAYY = SCRH - NAVH - BB_TRAYH - 6;
static const int BB_CELL  = (SCRW - 20) / GRID_N;         // 20 px on Pancake
static const int BB_X0    = (SCRW - BB_CELL * GRID_N) / 2;
static const int BB_Y0    = CONTENTY + 20;
// Name and premium counts share one line. They used to be two, which fit on the
// Pancake and overflowed the V8 by a row once the tray was added.
static const int BB_INFOY = CONTENTY + 4;

// The tray: four bonuses plus an eraser.
static const uint8_t BB_BRUSH[5] = { PR_DL, PR_TL, PR_DW, PR_TW, PR_NONE };
static uint8_t g_bbBrush = PR_DL;

static uint16_t bbFill(uint8_t p) {
  BoardPal P = theme.board();
  return p == PR_DL ? P.dl : p == PR_TL ? P.tl
       : p == PR_DW ? P.dw : p == PR_TW ? P.tw
       : p == PR_CENTRE ? P.dw : P.empty;
}
static const char *bbLabel(uint8_t p) {
  return p == PR_DL ? "DL" : p == PR_TL ? "TL"
       : p == PR_DW ? "DW" : p == PR_TW ? "TW"
       : p == PR_CENTRE ? "*" : "";
}

static void bbDrawCell(uint8_t r, uint8_t c) {
  BoardPal P = theme.board();
  Premium p = premiumAt(r, c);
  uint16_t fill = bbFill(p);
  int x = BB_X0 + c * BB_CELL, y = BB_Y0 + r * BB_CELL;
  tft->fillRect(x, y, BB_CELL - 1, BB_CELL - 1, fill);
  const char *lab = bbLabel(p);
  if (lab[0]) {
    tft->setTextColor(P.prem_text, fill);
    tft->setTextDatum(MC_DATUM);
    drawStr(lab, x + (BB_CELL - 1) / 2, y + (BB_CELL - 1) / 2, 1);
    tft->setTextDatum(TL_DATUM);
  }
}

// Name on the left, premium budget on the right.
static void bbDrawInfo() {
  uint8_t n[6];
  gridCounts(n);
  tft->fillRect(0, BB_INFOY - 2, SCRW, 18, COL_BG);
  tft->setTextDatum(TL_DATUM);
  tft->setTextColor(COL_FG, COL_BG);
  drawStr(g_gridName, BB_X0, BB_INFOY, 2);
  tft->setTextDatum(TR_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr(String(n[PR_DL]) + "/" + n[PR_TL] + "/" + n[PR_DW] + "/" + n[PR_TW],
          SCRW - BB_X0, BB_INFOY, 2);
  tft->setTextDatum(TL_DATUM);
}

static int bbTrayW() { return (SCRW - 2 * BB_X0) / 5; }

static void bbDrawTray() {
  BoardPal P = theme.board();
  int tw = bbTrayW();
  tft->fillRect(0, BB_TRAYY - 2, SCRW, BB_TRAYH + 4, COL_BG);
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t p = BB_BRUSH[i];
    int x = BB_X0 + i * tw, w = tw - 6;
    bool sel = (p == g_bbBrush);
    tft->fillRoundRect(x, BB_TRAYY, w, BB_TRAYH, 5, bbFill(p));
    // The selected brush is ringed in the accent colour; the others get the
    // board's own edge so the tray reads as part of the board.
    tft->drawRoundRect(x, BB_TRAYY, w, BB_TRAYH, 5, sel ? COL_ACCENT : P.tile_edge);
    if (sel) tft->drawRoundRect(x + 1, BB_TRAYY + 1, w - 2, BB_TRAYH - 2, 4, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(p == PR_NONE ? COL_DIM : P.prem_text, bbFill(p));
    drawStr(p == PR_NONE ? "X" : bbLabel(p), x + w / 2, BB_TRAYY + BB_TRAYH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
}

// Which tray swatch a tap landed on, or -1.
static int bbTrayHit(uint16_t x, uint16_t y) {
  if ((int)y < BB_TRAYY || (int)y >= BB_TRAYY + BB_TRAYH) return -1;
  int tw = bbTrayW();
  int i = ((int)x - BB_X0) / tw;
  if ((int)x < BB_X0 || i < 0 || i > 4) return -1;
  return i;
}

static void bbDrawAll() {
  tft->fillScreen(COL_BG);
  drawHeader("Board Builder", true);
  bbDrawInfo();
  for (uint8_t r = 0; r < GRID_N; r++)
    for (uint8_t c = 0; c < GRID_N; c++) bbDrawCell(r, c);
  bbDrawTray();
  drawNav("New", "Load", "Save");
}

static void boardBuilderScreen() {
  bbDrawAll();
  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    // The header chevron is the way out now that the footer's left slot is New.
    if (backTapped(x, y)) return;

    int nh = navHit(x, y);

    if (nh == 0) {                               // New — a blank board
      for (uint16_t i = 0; i < GRID_CELLS; i++) g_grid[i] = PR_NONE;
      g_grid[(GRID_N / 2) * GRID_N + GRID_N / 2] = PR_CENTRE;
      strncpy(g_gridName, "Untitled", GRID_NAME_MAX - 1);
      g_gridName[GRID_NAME_MAX - 1] = 0;
      bbDrawAll();
      continue;
    }

    if (nh == 1) {                               // Load
      String rows[GRID_MAX_SAVED + 4];
      uint8_t n = 0;
      for (uint8_t i = 0; i < gridBuiltinCount(); i++) rows[n++] = gridBuiltinName(i);
      uint8_t nb = n;
      String saved[GRID_MAX_SAVED];
      uint8_t ns = gridListSaved(saved, GRID_MAX_SAVED);
      for (uint8_t i = 0; i < ns && n < GRID_MAX_SAVED + 4; i++) rows[n++] = saved[i];
      int pick = scrollList("Load Board", rows, n, true);
      if (pick >= 0) {
        if (pick < (int)nb) gridLoadBuiltin((uint8_t)pick);
        else                gridLoad(rows[pick].c_str());
        cfgSave();
      }
      bbDrawAll();
      continue;
    }

    if (nh == 2) {                               // Save
      char nm[GRID_NAME_MAX] = {0};
      strncpy(nm, g_gridName, GRID_NAME_MAX - 1);
      // A built-in cannot be overwritten, so do not offer its name back as the
      // default and have the save land somewhere the Load list will not show.
      if (!strcmp(nm, "Classic") || !strcmp(nm, "Random")) nm[0] = 0;
      if (touchKeyboardInput(*tft, COL_FG, COL_BG, nm, sizeof(nm), "Board name:", false) && nm[0]) {
        if (gridSave(nm)) { cfgSave(); msgScreen("Board Builder", "Saved", nm, COL_OK); }
        else              msgScreen("Board Builder", "Could not save",
                                    "Check the SD card is present and writable.", TFT_RED);
      }
      bbDrawAll();
      continue;
    }

    int t = bbTrayHit(x, y);
    if (t >= 0) {
      g_bbBrush = BB_BRUSH[t];
      bbDrawTray();
      continue;
    }

    // A tap on the grid lays the selected bonus, and its mirrors with it.
    if ((int)x < BB_X0 || (int)y < BB_Y0) continue;
    int cx = ((int)x - BB_X0) / BB_CELL, cy = ((int)y - BB_Y0) / BB_CELL;
    if (cx >= GRID_N || cy >= GRID_N) continue;
    // Tapping a square that already holds the brush takes it back off, so one
    // brush covers both placing and removing without a trip to the eraser.
    uint8_t cur = premiumAt((uint8_t)cy, (uint8_t)cx);
    gridPaint((uint8_t)cy, (uint8_t)cx, (cur == g_bbBrush) ? PR_NONE : g_bbBrush);

    // Repaint only the squares the orbit could have touched, so the board never
    // flashes. The orbit lives entirely within rows/columns cy, cx and their
    // mirrors, so those four lines crossed with themselves cover it.
    const uint8_t m = GRID_N - 1;
    uint8_t lines[4] = { (uint8_t)cy, (uint8_t)(m - cy), (uint8_t)cx, (uint8_t)(m - cx) };
    for (uint8_t li = 0; li < 4; li++)
      for (uint8_t k = 0; k < 4; k++) {
        bbDrawCell(lines[li], lines[k]);
        bbDrawCell(lines[k], lines[li]);
      }
    bbDrawInfo();
  }
}

// Dictionary Editor — overrides on top of the loaded word list.
//
// Look a word up, then block it or allow it, and browse what you have changed.
// The list rows carry a remove action. What is stored is the edit, not a new
// dictionary — see wordedit.h, including the one real limitation (an allowed
// word is playable by you but unreachable by the opponent, which finds its
// moves by walking the DAWG).
static void dictListScreen(WordStatus which) {
  const char *title = (which == WS_BLOCKED) ? "Blocked Words" : "Allowed Words";
  for (;;) {
    uint8_t n = weCount(g_lang, which);
    if (!n) {
      msgScreen(title, "Nothing here yet",
                which == WS_BLOCKED
                  ? "Look a word up and block it to keep it out of play."
                  : "Look a word up and allow it to play a word the list lacks.",
                COL_DIM);
      return;
    }
    static String rows[WE_MAX_WORDS];
    for (uint8_t i = 0; i < n; i++) {
      uint8_t w[WE_MAX_LEN];
      uint8_t len = weAt(g_lang, which, i, w);
      rows[i] = wordDisplay(w, len);
    }
    int pick = scrollList(title, rows, n, true);
    if (pick < 0) return;
    uint8_t w[WE_MAX_LEN];
    uint8_t len = weAt(g_lang, which, (uint8_t)pick, w);
    if (len) weSet(g_lang, w, len, WS_NORMAL);   // the row's remove action
  }
}

static void dictionaryScreen() {
  if (!g_dawg.loaded() && !dictLoad(true)) {
    msgScreen("Dictionary Editor", "Word list not loaded",
              String(LANG_FILES[g_lang]) + " - " + g_dawg.error() +
              ". Copy the .dwg files to " SCRABBLE_DIR " on the SD card.", TFT_RED);
    return;
  }

  char buf[24] = {0};
  uint8_t w[WE_MAX_LEN];
  uint8_t wn = 0;
  bool repaint = true;
  // Looked up once per word, not once per repaint: the offline lookup bisects a
  // multi-megabyte file on the SD card, and blocking/allowing repaints often.
  String def;
  bool defOnline = false;

  for (;;) {
   if (repaint) {
    repaint = false;
    tft->fillScreen(COL_BG);
    drawHeader("Dictionary Editor", true);

    int y = CONTENTY + 14;
    tft->setTextDatum(TL_DATUM);
    tft->setTextColor(COL_DIM, COL_BG);
    drawStr(String(LANG_NAMES[g_lang]) + " word list", 12, y, 2); y += 22;
    drawStr(String(g_dawg.wordCount()) + " words", 12, y, 2); y += 22;
    drawStr(String("blocked ") + weCount(g_lang, WS_BLOCKED) +
            "   allowed " + weCount(g_lang, WS_ALLOWED), 12, y, 2); y += 28;

    if (wn) {
      bool inDict = g_dawg.contains(w, wn);
      WordStatus st = weStatus(g_lang, w, wn);
      bool playable = wordAllowed(g_lang, w, wn, inDict);

      tft->setTextDatum(MC_DATUM);
      tft->setTextColor(COL_FG, COL_BG);
      drawStr(wordDisplay(w, wn), SCRW / 2, SCRH / 2 - 16, 4);
      tft->setTextColor(playable ? COL_OK : TFT_RED, COL_BG);
      drawStr(playable ? "PLAYABLE" : "NOT PLAYABLE", SCRW / 2, SCRH / 2 + 14, 2);
      tft->setTextColor(COL_DIM, COL_BG);
      drawStr(st == WS_BLOCKED ? "blocked by you"
            : st == WS_ALLOWED ? "allowed by you"
            : inDict           ? "in the word list"
                               : "not in the word list",
              SCRW / 2, SCRH / 2 + 36, 2);
      tft->setTextDatum(TL_DATUM);

      // The definition, wrapped into whatever room is left above the footer.
      int dy = SCRH / 2 + 54;
      const int dbot = SCRH - NAVH - 6, dlh = 17;
      tft->setTextColor(COL_DIM, COL_BG);
      drawStr(def.length() ? (defOnline ? "online" : "SD dictionary") : "no definition",
              12, dy, 1);
      dy += 14;
      tft->setTextColor(COL_FG, COL_BG);
      String cur;
      const int maxW = SCRW - 24;
      for (int i = 0; i <= (int)def.length() && dy + dlh <= dbot; i++) {
        char ch = (i < (int)def.length()) ? def[i] : (char)0x0A;
        if (ch == 0x0A) {
          if (cur.length()) { drawStr(cur, 12, dy, 2); dy += dlh; }
          cur = "";
          continue;
        }
        if (ch == ' ' && !cur.length()) continue;
        String cand = cur + ch;
        if (strWidth(cand, 2) > maxW) {
          int sp = cur.lastIndexOf(' ');
          if (sp > 0) { drawStr(cur.substring(0, sp), 12, dy, 2); cur = cur.substring(sp + 1) + ch; }
          else        { drawStr(cur, 12, dy, 2); cur = String(ch); }
          dy += dlh;
        } else {
          cur = cand;
        }
      }
      if (cur.length() && dy + dlh <= dbot) drawStr(cur, 12, dy, 2);
    } else if (buf[0]) {
      tft->setTextDatum(MC_DATUM);
      tft->setTextColor(TFT_RED, COL_BG);
      drawStr("not a playable word", SCRW / 2, SCRH / 2, 2);
      tft->setTextDatum(TL_DATUM);
    }

    // The middle action is whichever change makes sense for the word on screen.
    const char *mid = "Look Up";
    if (wn) mid = (weStatus(g_lang, w, wn) != WS_NORMAL) ? "Reset"
                : g_dawg.contains(w, wn)                 ? "Block" : "Allow";
    drawNav("Back", mid, wn ? "Clear" : "Lists");
   }

    uint16_t x, ty;
    if (!waitTap(x, ty)) continue;
    if (backTapped(x, ty)) return;
    int nh = navHit(x, ty);
    if (nh == 0) return;

    if (nh == 1) {
      if (wn) {
        WordStatus st = weStatus(g_lang, w, wn);
        weSet(g_lang, w, wn, st != WS_NORMAL ? WS_NORMAL
                           : g_dawg.contains(w, wn) ? WS_BLOCKED : WS_ALLOWED);
      } else {
        if (touchKeyboardInput(*tft, COL_FG, COL_BG, buf, sizeof(buf), "Look up word:", false)) {
          wn = normaliseWord(buf, w, sizeof(w));
          def = ""; defOnline = false;
          if (wn) {
            ledBusy();
            defineWord(g_lang, w, wn, WiFi.status() == WL_CONNECTED, def, defOnline);
            ledOff();
          }
        } else { buf[0] = 0; wn = 0; def = ""; }
      }
      repaint = true;
      continue;
    }

    if (nh == 2) {
      // With a word on screen the right button clears it, so the keyboard is
      // reachable again without leaving the screen.
      if (wn) { wn = 0; buf[0] = 0; def = ""; repaint = true; continue; }
      String rows[2] = { String("Blocked (") + weCount(g_lang, WS_BLOCKED) + ")",
                         String("Allowed (") + weCount(g_lang, WS_ALLOWED) + ")" };
      int pick = scrollList("Edited Words", rows, 2, true);
      if (pick == 0) dictListScreen(WS_BLOCKED);
      if (pick == 1) dictListScreen(WS_ALLOWED);
      repaint = true;
    }
  }
}

// Letter Distribution editor — the point value and bag count of every tile, per
// language.
//
// The list scrolls (there are 27-30 rows) and is composited into a sprite, so
// holding a stepper doesn't strobe the screen. Edits apply to the live table
// immediately and are saved on the way out; they take effect on the NEXT new
// game, since a game in progress already has its bag filled.
static const int DST_ROWH = 36;
static const int DST_CHIPW = 26, DST_CHIPH = 22;
static const int DST_VM = 60,  DST_VP = 124;    // value  [-] / [+]
static const int DST_BM = 184, DST_BP = 248;    // bag    [-] / [+]

// Which stepper a tap hit: 0 val-, 1 val+, 2 bag-, 3 bag+, -1 none.
static int dstChipAt(int x, int yInRow) {
  if (yInRow < (DST_ROWH - DST_CHIPH) / 2 ||
      yInRow >= (DST_ROWH + DST_CHIPH) / 2) return -1;
  if (x >= DST_VM && x < DST_VM + DST_CHIPW) return 0;
  if (x >= DST_VP && x < DST_VP + DST_CHIPW) return 1;
  if (x >= DST_BM && x < DST_BM + DST_CHIPW) return 2;
  if (x >= DST_BP && x < DST_BP + DST_CHIPW) return 3;
  return -1;
}

// Row 0 of the list is the all-seven-tiles bonus, which has a value but no bag
// count — so its bag steppers are omitted rather than drawn dead.
static void dstBingoRow(TFT_eSprite &g, const uint8_t *&track, int y, uint8_t lang) {
  g.fillRect(0, y, SCRW, DST_ROWH, COL_BG);
  g.setTextColor(COL_FG, COL_BG);
  g.setTextDatum(ML_DATUM);
  sprStr(g, track, "BINGO", 12, y + DST_ROWH / 2, 2);

  int by = y + (DST_ROWH - DST_CHIPH) / 2;
  const int xs[2] = { DST_VM, DST_VP };
  for (int k = 0; k < 2; k++) {
    g.fillRoundRect(xs[k], by, DST_CHIPW, DST_CHIPH, 4, COL_ACCENT);
    g.drawRoundRect(xs[k], by, DST_CHIPW, DST_CHIPH, 4, COL_DIM);
    int cx = xs[k] + DST_CHIPW / 2, cy = by + DST_CHIPH / 2;
    g.fillRect(cx - 6, cy - 1, 12, 2, COL_FG);
    if (k & 1) g.fillRect(cx - 1, cy - 6, 2, 12, COL_FG);
  }
  g.setTextDatum(MC_DATUM);
  g.setTextColor(COL_FG, COL_BG);
  sprStr(g, track, String((int)g_dist.bingo(lang)),
         (DST_VM + DST_CHIPW + DST_VP) / 2, y + DST_ROWH / 2, 2);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(COL_DIM, COL_BG);
  sprStr(g, track, "all 7 tiles", DST_BM, y + DST_ROWH / 2, 1);
  g.drawFastHLine(0, y + DST_ROWH - 1, SCRW, theme.edge());
  g.setTextDatum(TL_DATUM);
}

static void dstRow(TFT_eSprite &g, const uint8_t *&track, int y, uint8_t lang, uint8_t i) {
  const TileDef &d = g_dist.at(lang, i);
  g.fillRect(0, y, SCRW, DST_ROWH, COL_BG);

  // Letter. Blanks have no glyph of their own, so show them as "?" like a tile.
  g.setTextColor(COL_FG, COL_BG);
  g.setTextDatum(ML_DATUM);
  sprStr(g, track, d.letter == TILE_BLANK ? String("?") : String((char)d.letter),
         12, y + DST_ROWH / 2, 4);

  int by = y + (DST_ROWH - DST_CHIPH) / 2;
  const int xs[4] = { DST_VM, DST_VP, DST_BM, DST_BP };
  for (int k = 0; k < 4; k++) {
    g.fillRoundRect(xs[k], by, DST_CHIPW, DST_CHIPH, 4, COL_ACCENT);
    g.drawRoundRect(xs[k], by, DST_CHIPW, DST_CHIPH, 4, COL_DIM);
    // "-" on the left of each pair, "+" on the right.
    int cx = xs[k] + DST_CHIPW / 2, cy = by + DST_CHIPH / 2;
    g.fillRect(cx - 6, cy - 1, 12, 2, COL_FG);
    if (k & 1) g.fillRect(cx - 1, cy - 6, 2, 12, COL_FG);
  }

  g.setTextDatum(MC_DATUM);
  g.setTextColor(COL_FG, COL_BG);
  sprStr(g, track, String((int)d.value), (DST_VM + DST_CHIPW + DST_VP) / 2, y + DST_ROWH / 2, 2);
  sprStr(g, track, String((int)d.count), (DST_BM + DST_CHIPW + DST_BP) / 2, y + DST_ROWH / 2, 2);

  g.drawFastHLine(0, y + DST_ROWH - 1, SCRW, theme.edge());
  g.setTextDatum(TL_DATUM);
}

static void letterDistScreen() {
  uint8_t lang = g_lang;
  const int CY = CONTENTY + 22;                       // below the column headings
  const int CH = SCRH - CY - NAVH;
  const uint8_t n = g_dist.count(lang) + 1;   // +1 for the BINGO row at the top
  const int total = n * DST_ROWH;

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  const uint8_t *sf = nullptr;

  // Column headings + running tile total. Redrawn only when the total changes.
  auto header = [&]() {
    tft->fillRect(0, CONTENTY, SCRW, 22, COL_BG);
    tft->setTextDatum(ML_DATUM);
    tft->setTextColor(COL_DIM, COL_BG);
    drawStr("VALUE", DST_VM, CONTENTY + 11, 1);
    drawStr("BAG",   DST_BM, CONTENTY + 11, 1);
    uint16_t tot = g_dist.totalTiles(lang);
    tft->setTextDatum(MR_DATUM);
    // Over the engine's bag capacity the extra tiles would simply never be
    // dealt, so flag it rather than silently truncating.
    tft->setTextColor(tot > DIST_MAX_TILES ? TFT_RED : COL_DIM, COL_BG);
    drawStr(String(tot) + " tiles", SCRW - 8, CONTENTY + 11, 1);
    tft->setTextDatum(TL_DATUM);
  };

  float scroll = 0, fling = 0;
  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (!haveSpr) return;
    sf = nullptr;
    spr.fillSprite(COL_BG);
    for (uint8_t i = 0; i < n; i++) {
      int y = i * DST_ROWH - (int)scroll;
      if (y + DST_ROWH < 0 || y > CH) continue;
      if (i == 0) dstBingoRow(spr, sf, y, lang);
      else        dstRow(spr, sf, y, lang, i - 1);
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };

  tft->fillScreen(COL_BG);
  drawHeader(String("Letters: ") + LANG_NAMES[lang], true);
  header();
  drawNav("Back", "Reset", "Done");
  render();

  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0, repeatT = 0;
  int heldChip = -1, heldRow = -1;

  auto applyStep = [&](int row, int chip) {
    if (row == 0) {                                  // BINGO row: value only
      int v = (int)g_dist.bingo(lang);
      if (chip == 0) v -= DIST_BINGO_STEP;
      else if (chip == 1) v += DIST_BINGO_STEP;
      else return;                                   // no bag steppers here
      if (v < 0) v = 0;
      g_dist.setBingo(lang, (uint16_t)v);
      render();
      return;
    }
    TileDef &d = g_dist.at(lang, row - 1);
    switch (chip) {
      case 0: if (d.value > 0) d.value--; break;
      case 1: if (d.value < DIST_MAX_VALUE) d.value++; break;
      case 2: if (d.count > 0) d.count--; break;
      case 3: if (d.count < DIST_MAX_COUNT) d.count++; break;
    }
    render();
    header();
  };

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0;
      lastY = ty; lastT = now; vel = 0;
      heldChip = -1; heldRow = -1;
      if ((int)ty >= CY && (int)ty < CY + CH) {
        int abs = (int)ty - CY + (int)scroll;
        int row = abs / DST_ROWH;
        int chip = dstChipAt(tx, abs % DST_ROWH);
        if (row >= 0 && row < n && chip >= 0) {
          heldRow = row; heldChip = chip;
          applyStep(row, chip);
          repeatT = now + 450;                 // hold to repeat
        }
      }
    } else if (down && wasDown) {
      if (heldChip >= 0) {
        // Auto-repeat while held, so a count can be walked up quickly.
        if (now > repeatT) { applyStep(heldRow, heldChip); repeatT = now + 90; }
      } else {
        int dy = (int)pY - (int)ty;
        if (abs(dy) > 6) moved = true;
        if (moved) {
          scroll = pScroll + dy;
          uint32_t dt = now - lastT;
          if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
          render();
        }
      }
    } else if (!down && wasDown) {
      if (heldChip >= 0) { heldChip = -1; }
      else if (!moved) {
        if (backTapped(pX, pY)) break;
        int nh = navHit(pX, pY);
        if (nh == 0 || nh == 2) break;         // Back / Done both save
        if (nh == 1) {                         // Reset to the built-in table
          g_dist.reset(lang);
          render();
          header();
        }
      } else {
        fling = vel;
      }
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      render();
    } else {
      fling = 0;
    }

    wasDown = down;
    delay(10);
  }

  if (haveSpr) spr.deleteSprite();
  g_dist.save(lang);
}

// Settings (H4W9 layout: highlight on tap, partial redraw, no flash)
// Theme, Accent, Font Color, Board, Language, Letters, CPU Words, Tile Draw,
// Brightness, LED, WiFi, Debug, About
// (+ Calibrate Touch on resistive panels — capacitive needs no calibration).
#ifdef HAS_CAP_TOUCH
static const int SET_N = 14;
#else
static const int SET_N = 15;
#endif
// Rows 0..9 are chip rows; the value string is needed for hit-testing too.
// Row 5 (Letters) opens a screen, so it is an info row despite sitting here.
static const int SET_CHIP_MAX = 10;
static String setChipVal(int row) {
  switch (row) {
    case 0: return theme.themeName();
    case 1: return theme.accentName();
    case 2: return theme.fontColName();
    case 3: return theme.boardPalName();
    case 4: return LANG_NAMES[g_lang];
    // If the reduced list is switched on but missing from the card the CPU
    // silently plays from the full one — which looks exactly like the setting
    // not working. Say so instead.
    case 6: if (!g_cpuCommon) return "Full";
            return (g_dawg.loaded() && !g_dawgCpu.loaded()) ? "No list!" : "Human-like";
    case 7: return g_luckHelper ? "Balanced" : "Pure random";
    case 8: return (g_crossLimit == 1) ? String("1 (tidy)") : String(g_crossLimit);
    case 9: return String(theme.bright + 1) + "/20";
    case 10: return String(theme.led_bright) + "/20";
  }
  return "";
}
// Draw one settings row at its slot, highlighted if `sel`. No fillScreen.
static void drawSettingRow(int row, int sel, int y) {
  bool s = (row == sel);
  switch (row) {
    case 0: drawChipRow(y, "Theme",      setChipVal(0), false, s, 0); break;
    case 1: drawChipRow(y, "Accent",     setChipVal(1), false, s, 0); break;
    case 2: drawChipRow(y, "Font Color", setChipVal(2), false, s, theme.fontColPreview()); break;
    case 3: drawChipRow(y, "Board",      setChipVal(3), false, s, 0); break;
    case 4: drawChipRow(y, "Language",   setChipVal(4), false, s, 0); break;
    case 5: drawInfoRow(y, "Letters",    g_dist.edited(g_lang) ? String("edited") : String(""), s); break;
    case 6: drawChipRow(y, "CPU Words",  setChipVal(6), false, s, 0); break;
    case 7: drawChipRow(y, "Tile Draw",  setChipVal(7), false, s, 0); break;
    case 8: drawChipRow(y, "Cross Words", setChipVal(8), true, s, 0); break;
    case 9: drawChipRow(y, "Brightness", setChipVal(9), true,  s, 0); break;
    case 10: drawChipRow(y, "LED",       setChipVal(10), true, s, 0); break;
    case 11: drawInfoRow(y, "WiFi Setup", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""), s); break;
    case 12: drawInfoRow(y, "WiFi Debug", "", s); break;
    case 13: drawInfoRow(y, "About",     "", s); break;
#ifndef HAS_CAP_TOUCH
    case 14: drawInfoRow(y, "Calibrate Touch", "", s); break;
#endif
  }
}

// Sprite twin of drawSettingRow. Kept alongside rather than merged: the two
// renderers cannot share code because TFT_eSprite does not override the drawing
// primitives virtually.
static void sprSettingRow(TFT_eSprite &spr, const uint8_t *&sf, int row, int sel, int y) {
  bool s = (row == sel);
  switch (row) {
    case 0: sprChipRow(spr, sf, y, "Theme",      setChipVal(0), false, s, 0); break;
    case 1: sprChipRow(spr, sf, y, "Accent",     setChipVal(1), false, s, 0); break;
    case 2: sprChipRow(spr, sf, y, "Font Color", setChipVal(2), false, s, theme.fontColPreview()); break;
    case 3: sprChipRow(spr, sf, y, "Board",      setChipVal(3), false, s, 0); break;
    case 4: sprChipRow(spr, sf, y, "Language",   setChipVal(4), false, s, 0); break;
    case 5: sprInfoRow(spr, sf, y, "Letters",    g_dist.edited(g_lang) ? String("edited") : String(""), s); break;
    case 6: sprChipRow(spr, sf, y, "CPU Words",  setChipVal(6), false, s, 0); break;
    case 7: sprChipRow(spr, sf, y, "Tile Draw",  setChipVal(7), false, s, 0); break;
    case 8: sprChipRow(spr, sf, y, "Cross Words", setChipVal(8), true, s, 0); break;
    case 9: sprChipRow(spr, sf, y, "Brightness", setChipVal(9), true,  s, 0); break;
    case 10: sprChipRow(spr, sf, y, "LED",       setChipVal(10), true, s, 0); break;
    case 11: sprInfoRow(spr, sf, y, "WiFi Setup", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""), s); break;
    case 12: sprInfoRow(spr, sf, y, "WiFi Debug", "", s); break;
    case 13: sprInfoRow(spr, sf, y, "About",     "", s); break;
#ifndef HAS_CAP_TOUCH
    case 14: sprInfoRow(spr, sf, y, "Calibrate Touch", "", s); break;
#endif
  }
}

// About — app name/version/author, hardware + build detail rows, credits.
static void aboutScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("About", true);

#ifdef MARAUDER_V8
  const int dName = 26, dSub = 17, dAuth = 18, dRule = 6, dRow = 17, dGap = 2, dRule2 = 6, dCred = 16;
  const int valX = 82;
  const char *credit1 = "Word lists from dict.cc";
  const char *credit2 = "+ Wiktionary via Kaikki";
#else
  const int dName = 32, dSub = 22, dAuth = 24, dRule = 10, dRow = 21, dGap = 4, dRule2 = 8, dCred = 20;
  const int valX = 110;
  const char *credit1 = "Word lists from dict.cc + Wiktionary";
  const char *credit2 = "Classic 15x15 board";
#endif

  int cx = SCRW / 2, y = CONTENTY + 12;

  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr(FW_NAME, cx, y, 4); y += dName;
  drawStr(String("Version ") + FW_VERSION, cx, y, 2); y += dSub;
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("UI by " FW_AUTHOR, cx, y, 2); y += dAuth;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(1, theme.edge())); y += dRule;

  tft->setTextDatum(TL_DATUM);
  auto row = [&](const char *label, const String &value) {
    tft->setTextColor(COL_DIM, COL_BG); drawStr(label, 16, y, 2);
    tft->setTextColor(COL_FG, COL_BG);  drawStr(value, valX, y, 2);
    y += dRow;
  };
  row("Board",   BOARD_NAME);
  row("MCU",     BOARD_MCU);
  row("Display", BOARD_DISPLAY);
  row("Touch",   BOARD_TOUCH);
  {
    size_t ps = ESP.getPsramSize();
    if (ps >= 1024 * 1024)  row("PSRAM", String((unsigned)((ps + 512 * 1024) / (1024 * 1024))) + " MB");
    else if (ps > 0)        row("PSRAM", String((unsigned)(ps / 1024)) + " KB");
    else                    row("PSRAM", "None");
  }
  row("Words",   g_dawg.loaded() ? String(g_dawg.wordCount()) : String("not loaded"));
  row("Built",   __DATE__);
  row("Commit",  FW_COMMIT);

  y += dGap;
  tft->drawFastHLine(16, y, SCRW - 32, theme.neon(2, theme.edge())); y += dRule2;
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr(credit1, 16, y, 2); y += dCred;
  drawStr(credit2, 16, y, 2);

  statusLine("Tap to go back.", COL_DIM);
  uint16_t x, ty; waitTap(x, ty);
}

// Settings.
//
// The list scrolls, composited into a sprite like every other long list here.
// It has to: the row count grew past a screenful on the Pancake and was well
// past one on the V8, and redrawing rows straight to the panel mid-drag is the
// flashing this firmware avoids. The scrollbar is drawn INSIDE the sprite by
// sprScrollBar, so it moves with the content in the same push and never
// strobes on its own.
static void settingsFlow() {
  int sel = -1;
  const int CY = CONTENTY;
  const int CH = SCRH - CONTENTY;
  const int total = SET_N * ITEMH;

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  const uint8_t *sf = nullptr;

  float scroll = 0, fling = 0;

  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (!haveSpr) {
      // No room for the sprite (a V8 without PSRAM): draw straight to the
      // panel. Scrolling still works, it just is not flicker-free. The viewport
      // clips it to the content area so a row scrolled half off the top cannot
      // paint over the header.
      tft->setViewport(0, CY, SCRW, CH);
      tft->fillRect(0, 0, SCRW, CH, COL_BG);
      for (int i = 0; i < SET_N; i++) {
        int y = i * ITEMH - (int)scroll;
        if (y + ITEMH < 0 || y > CH) continue;
        drawSettingRow(i, sel, y);
      }
      tft->resetViewport();
      return;
    }
    sf = nullptr;
    spr.fillSprite(COL_BG);
    for (int i = 0; i < SET_N; i++) {
      int y = i * ITEMH - (int)scroll;
      if (y + ITEMH < 0 || y > CH) continue;
      sprSettingRow(spr, sf, i, sel, y);
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };

  auto full = [&]() {                         // full repaint (theme/bg changed)
    tft->fillScreen(COL_BG);
    drawHeader("Settings", true);
    render();
  };
  auto recolor = [&]() {                      // font colour changed — no fillScreen
    drawHeader("Settings", true);
    render();
  };
  full();

  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;

  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t tx = touch->x(), ty = touch->y();
    uint32_t now = millis();

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0;
      lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      if (moved) {
        scroll = pScroll + dy;
        uint32_t dt = now - lastT;
        if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
        render();
      }
    } else if (!down && wasDown) {
      if (moved) { fling = vel; wasDown = down; delay(10); continue; }

      if (backTapped(pX, pY)) { if (haveSpr) spr.deleteSprite(); ledOff(); return; }
      if ((int)pY < CY) { wasDown = down; delay(10); continue; }

      // Screen y -> list row, through the scroll offset.
      int absY = (int)pY - CY + (int)scroll;
      int row = absY / ITEMH;
      if (row < 0 || row >= SET_N) { wasDown = down; delay(10); continue; }

      int old = sel; sel = row;
      if (old != row) render();
      if (row != 10) ledOff();                    // LED preview only on the LED row

      // Chip hit-testing works in the row's own space, so it is independent of
      // where the row currently sits on screen.
      int rowTop = CY + row * ITEMH - (int)scroll;
      int h = (row <= SET_CHIP_MAX) ? chipHit(rowTop, setChipVal(row), pX, pY) : -1;

      switch (row) {
        case 0: if (h >= 0) { theme.cycleTheme(h); theme.save(); applyThemeToViewManager(); full(); } break;
        case 1: if (h >= 0) { theme.cycleAccent(h); theme.save(); applyThemeToViewManager(); render(); } break;
        case 2: if (h >= 0) { theme.cycleFontCol(h); theme.save(); applyThemeToViewManager(); recolor(); } break;
        case 3: if (h >= 0) { theme.cycleBoardPal(h); theme.save(); render(); } break;
        // Changing language invalidates the loaded word list — drop it so the next
        // Dictionary/New Game load picks up the other .dwg.
        case 4: if (h >= 0) { g_lang = (g_lang == LANG_DE) ? LANG_EN : LANG_DE;
                              cfgSave(); g_dawg.unload(); g_dawgCpu.unload(); render(); } break;
        case 5: letterDistScreen(); full(); break;
        // Reduced vocabulary for the CPU. Loaded lazily, so just flip the flag.
        case 6: if (h >= 0) { g_cpuCommon = !g_cpuCommon; cfgSave(); render(); } break;
        // Balanced draws: see Game::shuffleBag/refill.
        // Applied by newGame()/loadGame() — it changes how the bag is built, so it
        // takes effect from the next game rather than mid-hand.
        case 7: if (h >= 0) { g_luckHelper = !g_luckHelper; cfgSave(); render(); } break;
        // How many parallel words one play may form. 1 keeps the board a
        // crossword; raising it allows the dense side-by-side plays that make a
        // board sloppy, so it stays opt-in. Takes effect on the next new game.
        case 8: if (h == 0 && g_crossLimit > 1) g_crossLimit--;
                else if (h == 1 && g_crossLimit < 5) g_crossLimit++;
                if (h >= 0) { cfgSave(); render(); } break;
        case 9: if (h == 0 && theme.bright > 0)  theme.bright--;
                else if (h == 1 && theme.bright < 19) theme.bright++;
                if (h >= 0) { theme.save(); applyBrightness(); render(); } break;
        case 10: if (h == 0 && theme.led_bright > 0)  theme.led_bright--;
                 else if (h == 1 && theme.led_bright < 20) theme.led_bright++;
                 if (h >= 0) { theme.save(); render(); }
                 ledWifi(); break;                // live preview at the new brightness
        case 11: wifiSetup(); full(); break;
        case 12: wifiDebug(); full(); break;
        case 13: aboutScreen(); full(); break;
#ifndef HAS_CAP_TOUCH
        case 14: touchCalRun(); full(); break;    // resistive drifts — allow a redo
#endif
        default: break;
      }
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      render();
    } else {
      fling = 0;
    }

    wasDown = down;
    delay(10);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Game view — PORTRAIT.
//
//  EVERYTHING is composited into off-screen sprites and pushed in one go. No
//  part of this screen ever draws straight to the panel, because any direct
//  fill/redraw is visible as a flash. Two sprites rather than one full-screen
//  buffer so that panning the board doesn't have to re-push the rack, and
//  touching the rack doesn't have to re-push the board:
//
//    g_bs  board viewport   320 x 315   (~200 KB, PSRAM)
//    g_ls  hint+rack+score  320 x  82   (~52 KB)
//
//  The header and banner are painted once per turn and left alone.
//
//  Board zoom: tapping an empty square toggles a ~1.9x zoom centred on it, and
//  while zoomed a drag pans. Unzoomed, all 15 columns fit in 21 px cells; zoomed
//  they are 40 px and about 8 columns are visible.
//
//    header  28   shell header: back chevron, turn, status corner
//    banner  20   "CPU played HOE and scored 12 points"
//    board  315   viewport onto a 315 px (fit) or 600 px (zoomed) board
//    hint    18   live word + score, or word-hint chips
//    rack    44   7 tiles
//    score   18   You / opponent / bag
//    nav     28   shell footer, contextual
// ═════════════════════════════════════════════════════════════════════════════
static Game g_game;
static bool g_gameActive = false;          // a game exists (Continue is meaningful)

// Pancake's edge-to-edge board (21 px cells) leaves the chrome below it room
// to spare on its 480 px screen. The same edge-to-edge sizing on V8's 320 px
// screen (15 px cells, full-size chrome) ran the score line about 30 px off
// the bottom. V8 gets a deliberately smaller, letterboxed board plus a
// compressed strip -- 26 px rack tiles match the touch-safe row height
// already used for ITEMH above -- so the whole thing clears the nav bar.
#ifdef MARAUDER_V8
static const int G_CELL  = 12;                     // letterboxed, not edge-to-edge
static const int G_BANH  = 14;
static const int G_HINTH = 16;                     // room for the 14px VLW font
static const int G_RT    = 26;                     // touch-safe min, not (SCRW-8)/RACK_N
#else
static const int G_CELL  = (SCRW - 4) / BOARD_N;   // fit-to-width cell (21 on Pancake)
static const int G_BANH  = 20;
static const int G_HINTH = 18;
static const int G_RT    = (SCRW - 8) / RACK_N;    // rack tile size
#endif
static const int G_CELLZ = G_CELL * 19 / 10;       // zoomed cell (~1.9x)
static const int G_BSZ   = G_CELL * BOARD_N;       // viewport height = fitted board
static const int G_BANY  = HDRH;
static const int G_BY    = G_BANY + G_BANH + 2;    // board viewport top
static const int G_LSY   = G_BY + G_BSZ + 2;       // lower strip top
static const int G_LSH   = G_HINTH + G_RT + G_HINTH + 4;
static const int G_RACKX = (SCRW - RACK_N * G_RT) / 2;

// Sprite framebuffers. Null if PSRAM was unavailable — see gSpritesBegin().
static TFT_eSprite *g_bs = nullptr;
static TFT_eSprite *g_ls = nullptr;
static const uint8_t *g_bsFont = nullptr;
static const uint8_t *g_lsFont = nullptr;

static int  g_selRack = -1;                // rack slot picked up by tapping
static String g_banner;
static uint16_t g_bannerCol = 0;

// Zoom / pan state. Pan is in board pixels, always clamped by gClampPan().
static bool g_zoom  = false;
static int  g_panX  = 0, g_panY = 0;

// Drag state. g_dragRack >= 0 means a rack tile is riding the finger.
static int  g_dragRack = -1;
static int  g_dragX = 0, g_dragY = 0;      // finger, in screen coords
static int  g_dragSlot = -1;               // tray slot the drag is hovering, -1 = none
static bool g_dragFromBoard = false;       // the drag started by lifting a placed tile
// A swipe that began on a committed tile: it looks a word up instead of panning.
static bool    g_swipeFrom = false;
static uint8_t g_swipeR = 0, g_swipeC = 0;

static inline int gCell()    { return g_zoom ? G_CELLZ : G_CELL; }
static inline int gBoardPx() { return gCell() * BOARD_N; }
// Board origin inside the viewport sprite: centred when it fits, else -pan.
static inline int gOffX() { int b = gBoardPx(); return (b <= SCRW)  ? (SCRW - b) / 2  : -g_panX; }
static inline int gOffY() { int b = gBoardPx(); return (b <= G_BSZ) ? (G_BSZ - b) / 2 : -g_panY; }

static void gClampPan() {
  int b = gBoardPx();
  int mx = b - SCRW,  my = b - G_BSZ;
  if (mx < 0) mx = 0;
  if (my < 0) my = 0;
  if (g_panX < 0) g_panX = 0;
  if (g_panY < 0) g_panY = 0;
  if (g_panX > mx) g_panX = mx;
  if (g_panY > my) g_panY = my;
}
// Centre the viewport on a board cell (used when zooming in on a tap).
static void gCentreOn(int row, int col) {
  g_panX = col * gCell() + gCell() / 2 - SCRW / 2;
  g_panY = row * gCell() + gCell() / 2 - G_BSZ / 2;
  gClampPan();
}

static int rackSlotX(int i) { return G_RACKX + i * G_RT; }

// Whose tiles the tray displays. NOT necessarily whose turn it is: while the CPU
// is playing, the tray must keep showing the human's rack rather than swapping to
// the opponent's letters (which also gave away its hand). Interaction always
// happens on a human turn, where this and current() are the same player.
static uint8_t g_viewPlayer = 0;
static void updateViewPlayer() {
  if (!g_game.player(g_game.current()).isCpu) g_viewPlayer = g_game.current();
}
static inline const uint8_t *viewRack() { return g_game.player(g_viewPlayer).rack; }

// Words spelled by CONSECUTIVE tiles in the tray, shown above the rack
// ("vid id ma" from a C V I D N M A rack: V-I-D, I-D and M-A are each an adjacent
// run). Rearranging the tray changes them, which is the point — they reward
// lining letters up.
#define HINT_MAX 5
static String g_hint[HINT_MAX];
static uint8_t g_hintN = 0;

static void buildTrayHints() {
  g_hintN = 0;
  const Dawg &d = cpuDict();
  if (!d.loaded()) return;
  const uint8_t *rack = viewRack();

  for (uint8_t a = 0; a < RACK_N && g_hintN < HINT_MAX; a++) {
    uint8_t w[RACK_N], n = 0;
    for (uint8_t b = a; b < RACK_N; b++) {
      uint8_t t = rack[b];
      // A blank has no letter of its own and a tile on the board is not in the
      // tray, so either one ends the run.
      if (t == TILE_EMPTY || t == TILE_BLANK) break;
      bool onBoard = false;
      for (uint8_t k = 0; k < g_game.pendingCount(); k++)
        if (g_game.pending()[k].rackIdx == b) onBoard = true;
      if (onBoard) break;
      w[n++] = t;
      if (n < 2) continue;
      if (!d.contains(w, n)) continue;
      String s;
      for (uint8_t i = 0; i < n; i++) s += (char)w[i];
      bool dupe = false;
      for (uint8_t i = 0; i < g_hintN; i++) if (g_hint[i] == s) dupe = true;
      if (!dupe && g_hintN < HINT_MAX) g_hint[g_hintN++] = s;
    }
  }
}

// Which tray slot a point corresponds to, or -1 if it isn't over the tray. Used
// both for the live "tiles move aside" preview and for the actual drop, so the
// gap you see is always the slot you get.
static int raySlotAt(int x, int y) {
  int ry = G_LSY + G_HINTH + 2;
  if (y < ry - 10 || y >= ry + G_RT + 10) return -1;
  int slot = (x - G_RACKX + G_RT / 2) / G_RT;
  if (slot < 0) slot = 0;
  if (slot > RACK_N - 1) slot = RACK_N - 1;
  return slot;
}

// A tile's glyph is just its letter byte: the VLW fonts carry real Ä Ö Ü ß, and
// the smooth-font wrappers map the private codes to them.
static inline String tileGlyph(uint8_t letter) { return String((char)letter); }

static bool pendingIsBlankAt(uint8_t r, uint8_t c) {
  for (uint8_t i = 0; i < g_game.pendingCount(); i++)
    if (g_game.pending()[i].row == r && g_game.pending()[i].col == c)
      return g_game.pending()[i].isBlank;
  return false;
}

// Draw one tile face at an arbitrary position, into any sprite. Shared by the
// board, the rack and the tile being dragged, so they can never drift apart.
static void gTile(TFT_eSprite &g, const uint8_t *&track, int x, int y, int sz,
                  uint8_t letter, bool blank, uint16_t face, uint16_t edge,
                  uint16_t textc, uint16_t valc, uint8_t value) {
  int rad = sz >= 30 ? 4 : 2;
  g.fillRoundRect(x, y, sz - 1, sz - 1, rad, face);
  g.drawRoundRect(x, y, sz - 1, sz - 1, rad, edge);
  g.setTextColor(textc, face);
  g.setTextDatum(MC_DATUM);
  if (letter == TILE_BLANK) {
    sprStr(g, track, "?", x + sz / 2, y + sz / 2, sz >= 30 ? 4 : 2);
  } else {
    sprStr(g, track, tileGlyph(letter), x + sz / 2 - 1, y + sz / 2, sz >= 30 ? 4 : 2);
    if (!blank) {
      g.setTextDatum(BR_DATUM);
      g.setTextColor(valc, face);
      sprStr(g, track, String((int)value), x + sz - 3, y + sz - 2, 1);
    }
  }
  g.setTextDatum(TL_DATUM);
}

// Board viewport. Only the cells that intersect the viewport are drawn, so the
// cost is the same zoomed or not. Coordinates are sprite-relative; the sprite is
// pushed at G_BY.
static void gPaintBoard(TFT_eSprite &g, const uint8_t *&track) {
  BoardPal P = theme.board();
  const int cs = gCell(), ox = gOffX(), oy = gOffY();
  int c0 = ox >= 0 ? 0 : (-ox) / cs;
  int r0 = oy >= 0 ? 0 : (-oy) / cs;
  int c1 = c0 + SCRW / cs + 2;  if (c1 > BOARD_N) c1 = BOARD_N;
  int r1 = r0 + G_BSZ / cs + 2; if (r1 > BOARD_N) r1 = BOARD_N;

  for (int r = r0; r < r1; r++) {
    for (int c = c0; c < c1; c++) {
      int x = ox + c * cs, y = oy + r * cs;
      uint8_t letter = g_game.shownAt(r, c);
      if (letter == TILE_EMPTY) {
        Premium p = premiumAt(r, c);
        uint16_t fill = p == PR_DL ? P.dl : p == PR_TL ? P.tl
                      : p == PR_DW ? P.dw : p == PR_TW ? P.tw
                      : p == PR_CENTRE ? P.dw : P.empty;
        g.fillRoundRect(x, y, cs - 1, cs - 1, 2, fill);
        const char *lbl = p == PR_DL ? "DL" : p == PR_TL ? "TL"
                        : p == PR_DW ? "DW" : p == PR_TW ? "TW" : nullptr;
        // Only label a cell that is FULLY on screen. A part-visible cell at the
        // viewport edge clips the glyphs mid-height, and a clipped "L" loses its
        // foot and reads as "I" — which is where the stray "TI"/"DI" came from.
        bool whole = (x >= 0 && y >= 0 && x + cs <= SCRW && y + cs <= G_BSZ);
        if (lbl && whole) {
          g.setTextColor(P.prem_text, fill);
          g.setTextDatum(MC_DATUM);
          sprStr(g, track, lbl, x + cs / 2, y + cs / 2, cs >= 30 ? 2 : 1);
        } else if (p == PR_CENTRE) {
          g.fillCircle(x + cs / 2, y + cs / 2, cs / 6, P.prem_text);
        }
        g.setTextDatum(TL_DATUM);
      } else {
        bool pend = g_game.isPendingAt(r, c);
        uint16_t face = pend ? P.tile_hold
                      : g_game.wasLastMove(r, c) ? P.tile_last : P.tile;
        bool blank = pend ? pendingIsBlankAt(r, c) : g_game.isBlankAt(r, c);
        gTile(g, track, x, y, cs, letter, blank, face, P.tile_edge,
              P.tile_text, P.tile_val, g_game.letterValue(letter));
      }
    }
  }

  // A rack tile riding the finger is drawn last so it floats above the board.
  if (g_dragRack >= 0) {
    uint8_t t = viewRack()[g_dragRack];
    int sz = gCell() < G_RT ? G_RT : gCell();
    int x = g_dragX - sz / 2, y = g_dragY - G_BY - sz / 2 - 10;   // lift above the fingertip
    gTile(g, track, x, y, sz, t, false, P.tile_last, COL_SEL,
          P.tile_text, P.tile_val, g_game.letterValue(t));
  }
}

// Composite the board: into the sprite when we have it (no flicker), else
// straight to the panel (flickers, but the game still plays without PSRAM).
static void gRenderBoard() {
  if (!g_bs) return;
  g_bsFont = nullptr;
  g_bs->fillSprite(theme.board().gutter);
  gPaintBoard(*g_bs, g_bsFont);
  g_bs->pushSprite(0, G_BY);
}

// Lower strip: hint line, rack, score line.
static void gPaintLower(TFT_eSprite &g, const uint8_t *&track) {
  BoardPal P = theme.board();

  // Hint line: while placing it reports the word and score (Word Check);
  // otherwise it shows the words the tray currently spells.
  if (!g_game.pendingCount() && g_hintN) {
    int wsum = 0;
    for (uint8_t i = 0; i < g_hintN; i++) wsum += strWidth(g_hint[i], 1) + 14;
    int x = (SCRW - wsum) / 2;
    if (x < 2) x = 2;
    for (uint8_t i = 0; i < g_hintN; i++) {
      int cw = strWidth(g_hint[i], 1) + 10;
      g.fillRoundRect(x, 1, cw, G_HINTH - 3, 4, theme.edge());
      g.setTextColor(COL_FG, theme.edge());   // same as the footer buttons
      g.setTextDatum(MC_DATUM);
      sprStr(g, track, g_hint[i], x + cw / 2, G_HINTH / 2, 1);
      x += cw + 4;
    }
    g.setTextDatum(TL_DATUM);
  }
  if (g_game.pendingCount()) {
    String words;
    MoveErr e = g_game.validate();
    g.setTextDatum(MC_DATUM);
    if (e == MV_OK) {
      int sc = g_game.scorePending(&words);
      g.setTextColor(COL_OK, COL_BG);
      sprStr(g, track, words + "  +" + sc, SCRW / 2, G_HINTH / 2, 2);
    } else {
      g.setTextColor(COL_DIM, COL_BG);
      sprStr(g, track, e == MV_BAD_WORD ? "not a word" : "incomplete",
             SCRW / 2, G_HINTH / 2, 2);
    }
    g.setTextDatum(TL_DATUM);
  }

  // Rack. While a tile is being dragged over the tray the others shuffle along
  // to open a gap where it would land, so the drop position is visible before
  // letting go.
  int ry = G_HINTH + 2;
  int order[RACK_N], nord = 0;
  for (int i = 0; i < RACK_N; i++) {
    if (i == g_dragRack) continue;                 // the dragged tile is on the finger
    bool onBoard = false;
    for (uint8_t k = 0; k < g_game.pendingCount(); k++)
      if (g_game.pending()[k].rackIdx == i) onBoard = true;
    order[nord++] = (viewRack()[i] == TILE_EMPTY || onBoard)
                      ? -1 : i;                    // -1 renders as an empty slot
  }
  bool gapping = (g_dragRack >= 0 && g_dragSlot >= 0);
  int k = 0;
  for (int slot = 0; slot < RACK_N; slot++) {
    int x = rackSlotX(slot);
    int src = -1;
    if (gapping) {
      // Hovering the tray: close up the ranks and leave a gap at the target.
      if (slot != g_dragSlot && k < nord) src = order[k++];
    } else {
      // Not over the tray (idle, or dragging out over the board): tiles stay put
      // and the source slot simply reads empty.
      src = (slot == g_dragRack) ? -1 : slot;
    }
    if (src < 0 || viewRack()[src] == TILE_EMPTY) {
      g.drawRoundRect(x + 2, ry + 2, G_RT - 4, G_RT - 4, 4, theme.edge());
      continue;
    }
    bool onBoard = false;
    for (uint8_t kk = 0; kk < g_game.pendingCount(); kk++)
      if (g_game.pending()[kk].rackIdx == src) onBoard = true;
    if (onBoard) {
      g.drawRoundRect(x + 2, ry + 2, G_RT - 4, G_RT - 4, 4, theme.edge());
      continue;
    }
    uint8_t t = viewRack()[src];
    uint16_t face = (src == g_selRack) ? P.tile_last : P.tile;
    uint16_t edge = (src == g_selRack) ? COL_SEL : P.tile_edge;
    gTile(g, track, x + 1, ry + 1, G_RT - 1, t, false, face, edge,
          P.tile_text, P.tile_val, g_game.letterValue(t));
  }

  // Score line.
  int sy = ry + G_RT + 2;
  g.setTextDatum(ML_DATUM);
  int x = 6;
  for (uint8_t p = 0; p < g_game.numPlayers(); p++) {
    bool cur = (p == g_game.current());
    g.setTextColor(cur ? COL_OK : COL_DIM, COL_BG);
    String s = String(g_game.player(p).name) + " " + g_game.player(p).score;
    sprStr(g, track, s, x, sy + G_HINTH / 2, 2);
    x += strWidth(s, 2) + 10;
  }
  g.setTextColor(COL_DIM, COL_BG);
  g.setTextDatum(MR_DATUM);
  sprStr(g, track, String("Bag ") + g_game.bagCount(), SCRW - 6, sy + G_HINTH / 2, 2);
  g.setTextDatum(TL_DATUM);


  // The tile being dragged is painted into this sprite as well as the board's,
  // so it stays visible (and on top) once the finger moves over the tray.
  if (g_dragRack >= 0) {
    uint8_t t = viewRack()[g_dragRack];
    int sz = gCell() < G_RT ? G_RT : gCell();
    gTile(g, track, g_dragX - sz / 2, g_dragY - G_LSY - sz / 2 - 10, sz,
          t, false, P.tile_last, COL_SEL, P.tile_text, P.tile_val,
          g_game.letterValue(t));
  }
}

// Composite the lower strip, sprite or direct (see gRenderBoard).
static void gRenderLower() {
  if (!g_ls) return;
  updateViewPlayer();
  buildTrayHints();          // tray contents drive the chips, so refresh here
  g_lsFont = nullptr;
  g_ls->fillSprite(COL_BG);
  gPaintLower(*g_ls, g_lsFont);
  g_ls->pushSprite(0, G_LSY);
}

// Header + banner. Painted only when the turn or the message changes, so they
// contribute no flicker during play.
static void gRenderTop() {
  // "You" needs "Your turn", not "You's turn".
  {
    String who = g_game.player(g_game.current()).name;
    drawHeader(who == "You" ? String("Your turn") : who + "'s turn", true);
  }
  uint16_t bg = g_banner.length() ? (g_bannerCol ? g_bannerCol : COL_SEL) : COL_BG;
  tft->fillRect(0, G_BANY, SCRW, G_BANH, bg);
  if (g_banner.length()) {
    tft->setTextColor(contrastOn(bg), bg);
    tft->setTextDatum(MC_DATUM);
    drawStr(g_banner, SCRW / 2, G_BANY + G_BANH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
}

// Contextual footer, using the shell's own nav bar — it swaps when tiles are
// down.
//   tiles placed  ->  [Recall][Master][Play]
//   rack idle     ->  [Skip] [Master][Shuffle]
static void gRenderNav() {
  if (g_game.pendingCount()) drawNav("Master", "Recall", "Play");
  else                       drawNav("Master", "Shuffle", "Swap/Skip");
}

static void gRenderAll() {
  gRenderTop();
  gRenderBoard();
  gRenderLower();
  gRenderNav();
}

// Allocate the framebuffers. They live in PSRAM; without it we fall back to
// direct drawing, which flickers but still plays.
static bool gSpritesBegin() {
  g_bs = new TFT_eSprite(tft);
  g_bs->setColorDepth(16);
  if (!g_bs->createSprite(SCRW, G_BSZ)) { delete g_bs; g_bs = nullptr; }
  g_ls = new TFT_eSprite(tft);
  g_ls->setColorDepth(16);
  if (!g_ls->createSprite(SCRW, G_LSH)) { delete g_ls; g_ls = nullptr; }
  return g_bs && g_ls;
}
static void gSpritesEnd() {
  if (g_bs) { g_bs->deleteSprite(); delete g_bs; g_bs = nullptr; }
  if (g_ls) { g_ls->deleteSprite(); delete g_ls; g_ls = nullptr; }
}

// Screen point -> board cell. Returns false if the point misses the board.
static bool gCellAt(int sx, int sy, uint8_t &row, uint8_t &col) {
  if (sy < G_BY || sy >= G_BY + G_BSZ) return false;
  int bx = sx - gOffX(), by = (sy - G_BY) - gOffY();
  if (bx < 0 || by < 0) return false;
  int c = bx / gCell(), r = by / gCell();
  if (c < 0 || c >= BOARD_N || r < 0 || r >= BOARD_N) return false;
  row = (uint8_t)r; col = (uint8_t)c;
  return true;
}

// Ask which letter a blank should stand for.
static uint8_t blankPicker() {
  static const uint8_t DE_EXTRA[3] = { 0x80, 0x82, 0x84 };
  int n = 26 + (g_game.lang() == LANG_DE ? 3 : 0);
  const int cols = 5, cw = SCRW / cols, chh = 44;
  int y0 = CONTENTY + 30;

  tft->fillScreen(COL_BG);
  drawHeader("Blank", true);
  tft->setTextColor(COL_FG, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr("Blank stands for:", SCRW / 2, CONTENTY + 14, 2);
  for (int i = 0; i < n; i++) {
    int x = (i % cols) * cw, y = y0 + (i / cols) * chh;
    uint8_t l = (i < 26) ? (uint8_t)('A' + i) : DE_EXTRA[i - 26];
    tft->fillRoundRect(x + 3, y + 3, cw - 6, chh - 6, 4, COL_ACCENT);
    tft->setTextColor(COL_FG, COL_ACCENT);
    drawStr(tileGlyph(l), x + cw / 2, y + chh / 2, 4);
  }
  tft->setTextDatum(TL_DATUM);

  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    if (backTapped(x, y)) return 0;
    if ((int)y < y0) continue;
    int col = x / cw, row = ((int)y - y0) / chh;
    int i = row * cols + col;
    if (i >= 0 && i < n) return (i < 26) ? (uint8_t)('A' + i) : DE_EXTRA[i - 26];
  }
}

// Place a rack tile, prompting for the letter if it is a blank. Returns true if
// the tile ended up on the board.
static bool gPlaceFrom(int rackIdx, uint8_t r, uint8_t c) {
  if (rackIdx < 0 || !g_game.isEmpty(r, c)) return false;
  uint8_t declared = 0;
  if (g_game.player(g_game.current()).rack[rackIdx] == TILE_BLANK) {
    declared = blankPicker();
    tft->fillScreen(COL_BG);
    gRenderAll();
    if (!declared) return false;
  }
  return g_game.place(r, c, rackIdx, declared);
}

// scorePending() reports EVERY word a move forms ("HOE, THE, OX"). The banner
// names the move, so it wants only the primary one — the first listed, which is
// the word along the axis of play.
static String mainWord(const String &words) {
  int comma = words.indexOf(", ");
  return comma < 0 ? words : words.substring(0, comma);
}

// Swap / Skip — one dialog for both: pick the tiles to change, or pick none and
// just skip the turn. Returns true if the turn was used (either way), false if
// cancelled.
//
// Drawn ONCE, then only the tile that changed and the footer label are
// repainted. Redrawing the whole screen per tap is what made this flash.
static bool swapSkipDialog() {
  bool sel[RACK_N] = { false };
  const int tw = 40, gap = 4;
  const int ty = CONTENTY + 96;
  const int x0 = (SCRW - (RACK_N * tw + (RACK_N - 1) * gap)) / 2;
  BoardPal P = theme.board();

  auto drawTile = [&](int i) {
    uint8_t t = g_game.player(g_game.current()).rack[i];
    int x = x0 + i * (tw + gap);
    tft->fillRect(x, ty, tw, tw, COL_BG);
    if (t == TILE_EMPTY) { tft->drawRoundRect(x, ty, tw, tw, 4, theme.edge()); return; }
    uint16_t face = sel[i] ? P.tile_last : P.tile;
    tft->fillRoundRect(x, ty, tw, tw, 4, face);
    tft->drawRoundRect(x, ty, tw, tw, 4, sel[i] ? COL_SEL : P.tile_edge);
    tft->setTextColor(P.tile_text, face);
    tft->setTextDatum(MC_DATUM);
    drawStr(t == TILE_BLANK ? String("?") : tileGlyph(t), x + tw / 2, ty + tw / 2, 4);
    if (t != TILE_BLANK) {
      tft->setTextDatum(BR_DATUM);
      tft->setTextColor(P.tile_val, face);
      drawStr(String((int)g_game.letterValue(t)), x + tw - 3, ty + tw - 2, 1);
    }
    tft->setTextDatum(TL_DATUM);
  };
  auto anySel = [&]() {
    for (int i = 0; i < RACK_N; i++) if (sel[i]) return true;
    return false;
  };

  // Static part, painted once.
  tft->fillScreen(COL_BG);
  drawHeader("Swap/Skip", true);
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("Choose the tiles you want to change.", SCRW / 2, CONTENTY + 26, 2);
  drawStr("Select no tiles to just skip your turn.", SCRW / 2, CONTENTY + 48, 2);
  if (g_game.bagCount() < RACK_N) {
    tft->setTextColor(TFT_RED, COL_BG);
    drawStr("Too few tiles in the bag to swap.", SCRW / 2, ty + tw + 22, 2);
  }
  tft->setTextDatum(TL_DATUM);
  for (int i = 0; i < RACK_N; i++) drawTile(i);

  bool lastAny = false;
  drawNav("Select all", "", "Skip");

  for (;;) {
    uint16_t x, y;
    if (!waitTap(x, y)) continue;
    if (backTapped(x, y)) return false;

    if ((int)y >= ty && (int)y < ty + tw) {
      int i = ((int)x - x0) / (tw + gap);
      if (i >= 0 && i < RACK_N &&
          g_game.player(g_game.current()).rack[i] != TILE_EMPTY) {
        sel[i] = !sel[i];
        drawTile(i);                                  // just this tile
        if (anySel() != lastAny) {                    // and the footer label
          lastAny = anySel();
          drawNav("Select all", "", lastAny ? "Swap" : "Skip");
        }
      }
      continue;
    }

    int nh = navHit(x, y);
    if (nh == 0) {                                    // Select all
      for (int i = 0; i < RACK_N; i++) {
        bool want = (g_game.player(g_game.current()).rack[i] != TILE_EMPTY);
        if (sel[i] != want) { sel[i] = want; drawTile(i); }
      }
      if (anySel() != lastAny) {
        lastAny = anySel();
        drawNav("Select all", "", lastAny ? "Swap" : "Skip");
      }
      continue;
    }
    if (nh == 2) {
      if (!lastAny) { g_game.pass(); return true; }   // no tiles picked = skip
      if (g_game.exchange(sel)) return true;
      msgScreen("Swap", "Cannot swap",
                "The bag needs at least 7 tiles left.", TFT_RED);
      return false;
    }
  }
}

// Master: what the current rack could play, best first. Uses the generator's own
// ranked list and shows it as a scrolling list with each word's score — the
// previous version drew three words centred at font 4, which ran off the edges
// on anything long.
static bool masterScreen() {
  tft->fillScreen(COL_BG);
  drawHeader("Master", true);
  tft->setTextColor(COL_DIM, COL_BG);
  tft->setTextDatum(MC_DATUM);
  drawStr("Searching...", SCRW / 2, SCRH / 2, 2);
  tft->setTextDatum(TL_DATUM);

  // Anything laid out is recalled first: the generator needs a clean board, and
  // a chosen suggestion is going to replace it anyway.
  g_game.clearPending();

  // Suggestions come from the everyday word list, not the full one: a hint of
  // ZYMURGY is useless. Validation still accepts anything in the full list, so
  // this only narrows what is SUGGESTED.
  static CpuMove mv[12];
  uint8_t n = cpuFindMoves(g_game, cpuDict(), mv, 12, 4000, &g_dawg);

  if (!n) {
    msgScreen("Master", "No moves found",
              "No legal word can be made from this rack on this board.", COL_DIM);
    return false;
  }

  // Turn each move into "WORD   score", skipping duplicates of the same word.
  // `src` maps a displayed row back to the move that produced it, so picking a
  // row can lay that exact move out on the board.
  static String rows[12];
  static uint8_t src[12];
  uint8_t nr = 0;
  for (uint8_t i = 0; i < n && nr < 12; i++) {
    String w;
    cpuApply(g_game, mv[i]);
    g_game.scorePending(&w);
    g_game.clearPending();
    String word = mainWord(w);
    bool dupe = false;
    for (uint8_t k = 0; k < nr; k++) if (rows[k].startsWith(word + " ")) dupe = true;
    if (dupe) continue;
    src[nr] = i;
    rows[nr++] = word + "   " + mv[i].score;
  }

  int sel = scrollList("Master", rows, nr, true);
  if (sel < 0 || sel >= nr) return false;

  // Lay the chosen suggestion out as pending tiles, exactly where it plays. The
  // move is already legal on this board -- it came from the generator -- so the
  // player only has to press Play, or Recall to take it back.
  cpuApply(g_game, mv[src[sel]]);
  return true;
}

// Words played — every turn of the game, per player, with what it scored.
// Opened by tapping the score line under the rack.
//
// Two columns when it is a two-player game. With three or four players there is
// no room for a column each, so it falls back to one chronological list with the
// player's name on each row.
static const int WP_ROWH = 30;

static void wordsPlayedScreen() {
  const uint8_t np = g_game.numPlayers();
  const bool two = (np == 2);
  const int CY = CONTENTY + 22;                     // below the column headings
  const int CH = SCRH - CY - 30;                    // above the totals line

  // Per-player move lists, in order of play.
  static uint8_t idx[MAX_PLAYERS][MAX_HISTORY];
  uint8_t cnt[MAX_PLAYERS] = { 0 };
  for (uint8_t i = 0; i < g_game.historyCount(); i++) {
    uint8_t p = g_game.history(i).player;
    if (p < MAX_PLAYERS && cnt[p] < MAX_HISTORY) idx[p][cnt[p]++] = i;
  }
  int rows = 0;
  if (two) { rows = max(cnt[0], cnt[1]); }
  else     { rows = g_game.historyCount(); }
  const int total = rows * WP_ROWH;

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  const uint8_t *sf = nullptr;

  // A move with no word was a pass; show it as such.
  auto cell = [&](TFT_eSprite &g, int x, int w, int y, uint8_t h) {
    const MoveRec &m = g_game.history(h);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(m.score > 0 ? COL_OK : COL_DIM, COL_BG);
    sprStr(g, sf, String((int)m.score), x, y + WP_ROWH / 2, 2);
    g.setTextColor(COL_FG, COL_BG);
    sprStr(g, sf, m.word[0] ? String(m.word) : String("-"), x + 34, y + WP_ROWH / 2, 2);
  };

  float scroll = 0, fling = 0;
  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (!haveSpr) return;
    sf = nullptr;
    spr.fillSprite(COL_BG);
    for (int r = 0; r < rows; r++) {
      int y = r * WP_ROWH - (int)scroll;
      if (y + WP_ROWH < 0 || y > CH) continue;
      if (two) {
        if (r < cnt[0]) cell(spr, 10, SCRW / 2 - 14, y, idx[0][r]);
        if (r < cnt[1]) cell(spr, SCRW / 2 + 10, SCRW / 2 - 14, y, idx[1][r]);
        spr.drawFastVLine(SCRW / 2, y, WP_ROWH, theme.edge());
      } else {
        const MoveRec &m = g_game.history(r);
        spr.setTextDatum(ML_DATUM);
        spr.setTextColor(COL_DIM, COL_BG);
        sprStr(spr, sf, g_game.player(m.player).name, 10, y + WP_ROWH / 2, 2);
        cell(spr, 90, SCRW - 100, y, r);
      }
      spr.drawFastHLine(0, y + WP_ROWH - 1, SCRW, theme.edge());
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };

  tft->fillScreen(COL_BG);
  drawHeader("Words played", true);

  // Column headings, and the running totals along the bottom.
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_FG, COL_BG);
  if (two) {
    drawStr(g_game.player(0).name, SCRW / 4, CONTENTY + 11, 2);
    drawStr(g_game.player(1).name, SCRW * 3 / 4, CONTENTY + 11, 2);
  } else {
    drawStr("All turns", SCRW / 2, CONTENTY + 11, 2);
  }
  {
    int y = SCRH - 15;
    tft->setTextColor(COL_OK, COL_BG);
    if (two) {
      drawStr(String(g_game.player(0).score) + " points", SCRW / 4, y, 2);
      drawStr(String(g_game.player(1).score) + " points", SCRW * 3 / 4, y, 2);
    } else {
      String s;
      for (uint8_t p = 0; p < np; p++)
        s += String(g_game.player(p).name) + " " + g_game.player(p).score + "  ";
      drawStr(s, SCRW / 2, y, 2);
    }
  }
  tft->setTextDatum(TL_DATUM);

  if (!rows) {
    tft->setTextDatum(MC_DATUM);
    tft->setTextColor(COL_DIM, COL_BG);
    drawStr("No turns played yet.", SCRW / 2, SCRH / 2, 2);
    tft->setTextDatum(TL_DATUM);
  }
  render();

  // Scroll / dismiss.
  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0;
      lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      if (moved) {
        scroll = pScroll + dy;
        uint32_t dt = now - lastT;
        if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
        render();
      }
    } else if (!down && wasDown) {
      if (!moved) break;                    // any tap closes it
      fling = vel;
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      render();
    } else {
      fling = 0;
    }
    wasDown = down;
    delay(10);
  }
  if (haveSpr) spr.deleteSprite();
}

static const char *moveErrText(MoveErr e) {
  switch (e) {
    case MV_NO_TILES:      return "Place at least one tile first.";
    case MV_NOT_IN_LINE:   return "All tiles must be in one row or column.";
    case MV_HAS_GAP:       return "The word has a gap in it.";
    case MV_NOT_CENTRE:    return "The first word must cover the centre star.";
    case MV_NOT_CONNECTED: return "The word must touch a tile already played.";
    case MV_NO_WORD:       return "That makes no word at least two letters long.";
    case MV_TOO_MANY_CROSS: return "That touches too many words at once.";
    case MV_BAD_WORD:      return "Not in the word list:";
    default:               return "";
  }
}

// Definition viewer — reached by swiping along a word already on the board.
//
// Committed tiles only: a swipe that starts on one is a lookup, so panning still
// works by dragging from an empty square. The whole word under the swipe is read,
// not just the squares touched, so a quick flick across part of it is enough.
static void definitionScreen(const uint8_t *word, uint8_t len) {
  String w = wordDisplay(word, len);

  tft->fillScreen(COL_BG);
  drawHeader(w, true);
  tft->setTextDatum(MC_DATUM);
  tft->setTextColor(COL_DIM, COL_BG);
  drawStr("Looking up...", SCRW / 2, SCRH / 2, 2);
  tft->setTextDatum(TL_DATUM);

  String text;
  bool online = false;
  ledBusy();
  bool ok = defineWord(g_game.lang(), word, len,
                       WiFi.status() == WL_CONNECTED, text, online);
  ledOff();

  if (!ok) {
    msgScreen(w.c_str(), "No definition found",
              WiFi.status() == WL_CONNECTED
                ? "Not in the SD dictionary, and the online lookup had nothing."
                : "Not in the SD dictionary. Connect to WiFi to search online.",
              COL_DIM);
    return;
  }

  // Wrap into lines up front so the view can scroll without re-wrapping.
  static String line[64];
  uint8_t nline = 0;
  {
    String cur;
    int maxW = SCRW - 16;
    for (int i = 0; i <= (int)text.length() && nline < 64; i++) {
      char c = (i < (int)text.length()) ? text[i] : '\n';
      if (c == '\n') {
        if (cur.length()) line[nline++] = cur;
        cur = "";
        continue;
      }
      if (c == ' ' && !cur.length()) continue;
      String cand = cur + c;
      if (strWidth(cand, 2) > maxW) {
        int sp = cur.lastIndexOf(' ');
        if (sp > 0) {                        // break at the last space
          line[nline++] = cur.substring(0, sp);
          cur = cur.substring(sp + 1) + c;
        } else {
          line[nline++] = cur;
          cur = String(c);
        }
      } else {
        cur = cand;
      }
    }
    if (cur.length() && nline < 64) line[nline++] = cur;
  }

  const int LH = 20;
  const int CY = CONTENTY + 22;
  const int CH = SCRH - CY - NAVH;
  const int total = nline * LH;

  TFT_eSprite spr(tft);
  spr.setColorDepth(16);
  bool haveSpr = (spr.createSprite(SCRW, CH) != nullptr);
  const uint8_t *sf = nullptr;

  float scroll = 0, fling = 0;
  auto render = [&]() {
    float maxS = total > CH ? total - CH : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxS) scroll = maxS;
    if (!haveSpr) return;
    sf = nullptr;
    spr.fillSprite(COL_BG);
    spr.setTextDatum(ML_DATUM);
    spr.setTextColor(COL_FG, COL_BG);
    for (uint8_t i = 0; i < nline; i++) {
      int y = i * LH - (int)scroll;
      if (y + LH < 0 || y > CH) continue;
      sprStr(spr, sf, line[i], 8, y + LH / 2, 2);
    }
    sprScrollBar(spr, CH, total, scroll);
    spr.pushSprite(0, CY);
  };

  tft->fillScreen(COL_BG);
  drawHeader(w, true);
  tft->setTextDatum(ML_DATUM);
  tft->setTextColor(online ? COL_OK : COL_DIM, COL_BG);
  drawStr(online ? "online" : "SD dictionary", 8, CONTENTY + 11, 1);
  tft->setTextDatum(TL_DATUM);
  drawNav("Back", "", WiFi.status() == WL_CONNECTED && !online ? "Online" : "");
  render();

  bool wasDown = false, moved = false;
  uint16_t pX = 0, pY = 0, lastY = 0;
  float pScroll = 0, vel = 0;
  uint32_t lastT = 0;
  for (;;) {
    touch->run();
    bool down = touch->isPressed();
    uint16_t ty = touch->y(), tx = touch->x();
    uint32_t now = millis();

    if (down && !wasDown) {
      pX = tx; pY = ty; pScroll = scroll; moved = false; fling = 0;
      lastY = ty; lastT = now; vel = 0;
    } else if (down && wasDown) {
      int dy = (int)pY - (int)ty;
      if (abs(dy) > 6) moved = true;
      if (moved) {
        scroll = pScroll + dy;
        uint32_t dt = now - lastT;
        if (dt > 0) { vel = (float)((int)lastY - (int)ty) / (float)dt * 1000.0f; lastY = ty; lastT = now; }
        render();
      }
    } else if (!down && wasDown) {
      if (!moved) {
        if (backTapped(pX, pY)) break;
        int nh = navHit(pX, pY);
        if (nh == 0) break;
        if (nh == 2 && WiFi.status() == WL_CONNECTED && !online) {
          if (haveSpr) spr.deleteSprite();
          String t2;
          ledBusy();
          bool got = defineOnline(g_game.lang(), word, len, t2);
          ledOff();
          if (got) { definitionScreen(word, len); return; }   // redraw with the new text
          msgScreen(w.c_str(), "Online lookup failed",
                    "No result, or the request timed out.", TFT_RED);
          return;
        }
      } else {
        fling = vel;
      }
    } else if (fabs(fling) > 25) {
      scroll += fling * 0.016f;
      fling *= 0.95f;
      render();
    } else {
      fling = 0;
    }
    wasDown = down;
    delay(10);
  }
  if (haveSpr) spr.deleteSprite();
}

// Read the whole committed word lying along `horiz` through (r,c). Returns its
// length, or 0 if there is no word of 2+ letters there.
static uint8_t wordAt(uint8_t r, uint8_t c, bool horiz, uint8_t *out) {
  int8_t dr = horiz ? 0 : 1, dc = horiz ? 1 : 0;
  int sr = r, sc = c;
  while (true) {                              // walk back to the start
    int pr = sr - dr, pc = sc - dc;
    if (pr < 0 || pc < 0 || pr >= BOARD_N || pc >= BOARD_N) break;
    if (g_game.isEmpty((uint8_t)pr, (uint8_t)pc)) break;
    sr = pr; sc = pc;
  }
  uint8_t n = 0;
  while (sr >= 0 && sc >= 0 && sr < BOARD_N && sc < BOARD_N && n < 15) {
    if (g_game.isEmpty((uint8_t)sr, (uint8_t)sc)) break;
    out[n++] = g_game.at((uint8_t)sr, (uint8_t)sc);
    sr += dr; sc += dc;
  }
  return n >= 2 ? n : 0;
}

// Polled by the move generator while it searches. Tapping the back box during a
// long think cancels the search so the screen can be left — otherwise the panel
// is unresponsive for the whole budget and a slow turn looks like a hang.
static bool cpuAbortOnBackTap() {
  touch->run();
  return touch->isPressed() && backTapped(touch->x(), touch->y());
}

static void gameOver() {
  g_game.applyFinalScores();
  uint8_t w = g_game.leader();
  // "You" needs "You win", not "You wins".
  String wn = g_game.player(w).name;
  msgScreen("Game over", wn + (wn == "You" ? " win" : " wins"),
            String("Final score ") + g_game.player(w).score, COL_OK);
  g_gameActive = false;
}

// Main interactive loop.
static void gameScreen() {
  g_selRack = -1;
  g_dragRack = -1;
  g_dragSlot = -1;
  g_dragFromBoard = false;
  g_banner = "";
  g_zoom = false; g_panX = g_panY = 0;
  // The board renders only through the sprites, so a failed allocation is fatal
  // to this screen rather than merely ugly. On 8 MB of PSRAM it should never
  // happen — say so plainly instead of showing a blank board.
  if (!gSpritesBegin()) {
    Serial.printf("[Game] sprite alloc failed (board=%s lower=%s) free PSRAM %u\n",
                  g_bs ? "ok" : "FAIL", g_ls ? "ok" : "FAIL",
                  (unsigned)ESP.getFreePsram());
    gSpritesEnd();
    msgScreen("Game", "Out of PSRAM",
              String("Could not allocate the board buffers. Free PSRAM: ") +
              (ESP.getFreePsram() / 1024) + " KB.", TFT_RED);
    return;
  }
  tft->fillScreen(COL_BG);
  gRenderAll();

  // Touch tracking: press point, whether it became a drag, and what it grabbed.
  bool wasDown = false, moved = false;
  int  pressX = 0, pressY = 0, panX0 = 0, panY0 = 0;
  bool panning = false;

  for (;;) {
    // CPU turn.
    if (g_game.player(g_game.current()).isCpu) {
      g_banner = "thinking...";
      g_bannerCol = COL_ACCENT;
      gRenderTop();

      CpuMove mv;
      uint8_t lvl = g_game.player(g_game.current()).cpuLevel;
      String who = g_game.player(g_game.current()).name;

      // The search blocks for seconds; without a hook the panel is frozen and a
      // slow turn is indistinguishable from a crash.
      cpuSetAbortHook(cpuAbortOnBackTap);
      bool found = cpuFindMove(g_game, cpuDict(), lvl, mv, 4000, &g_dawg);
      bool cancelled = cpuWasAborted();
      cpuSetAbortHook(nullptr);

      if (cancelled) {
        g_game.clearPending();          // opponent has not moved; leave as-is
        SD.mkdir(SAVE_DIR);
        g_game.save(SAVE_DIR "/auto.sav");
        gSpritesEnd();
        return;
      }

      bool played = false;
      if (found) {
        String w;
        cpuApply(g_game, mv);
        g_game.scorePending(&w);
        gRenderBoard();                      // show the tiles before committing
        delay(700);
        // commit() re-validates and can refuse. If it ever does, pass rather
        // than looping on the same search forever — that stall is what made the
        // opponent appear stuck "thinking".
        if (g_game.commit()) {
          played = true;
          g_zoom = false; g_panX = g_panY = 0;   // show the move that was made
          g_banner = who + " played " + mainWord(w) + " (+" + mv.score + ")";
          g_bannerCol = COL_SEL;
        } else {
          Serial.println(F("[CPU] commit refused its own move; passing"));
          g_game.clearPending();
        }
      }
      if (!played) {
        g_game.pass();                       // always makes progress
        g_banner = who + " skipped";
        g_bannerCol = COL_ACCENT;
      }

      if (g_game.isOver()) { gSpritesEnd(); gameOver(); return; }
      gRenderAll();
      continue;
    }

    touch->run();
    bool down = touch->isPressed();
    int x = touch->x(), y = touch->y();

    // ── press ────────────────────────────────────────────────────────────────
    if (down && !wasDown) {
      pressX = x; pressY = y; moved = false; panning = false;
      panX0 = g_panX; panY0 = g_panY;
      // A swipe beginning on a tile already played is a definition lookup, so
      // note that here and don't let it turn into a pan.
      {
        uint8_t rr, cc;
        g_swipeFrom = (gCellAt(x, y, rr, cc) && !g_game.isEmpty(rr, cc) &&
                       !g_game.isPendingAt(rr, cc));
        g_swipeR = rr; g_swipeC = cc;
      }

      // Grabbing a tile already laid out this turn lifts it back off the board
      // and onto the finger, so it can be moved somewhere else or returned to
      // the tray. Only THIS turn's tiles — committed ones are fixed.
      uint8_t pr, pc;
      if (gCellAt(x, y, pr, pc) && g_game.isPendingAt(pr, pc)) {
        int idx = -1;
        for (uint8_t k = 0; k < g_game.pendingCount(); k++)
          if (g_game.pending()[k].row == pr && g_game.pending()[k].col == pc)
            idx = g_game.pending()[k].rackIdx;
        if (idx >= 0) {
          g_game.unplaceAt(pr, pc);
          g_dragRack = idx; g_dragX = x; g_dragY = y;
          g_dragSlot = -1;                 // came from the board, not the tray
          g_dragFromBoard = true;
          gRenderBoard(); gRenderLower(); gRenderNav();
          wasDown = down;
          delay(8);
          continue;
        }
      }

      // Grabbing a rack tile starts a drag immediately.
      int ry = G_LSY + G_HINTH + 2;
      if (y >= ry && y < ry + G_RT) {
        for (int i = 0; i < RACK_N; i++) {
          if (x < rackSlotX(i) || x >= rackSlotX(i) + G_RT) continue;
          uint8_t t = g_game.player(g_game.current()).rack[i];
          bool onBoard = false;
          for (uint8_t k = 0; k < g_game.pendingCount(); k++)
            if (g_game.pending()[k].rackIdx == i) onBoard = true;
          if (t != TILE_EMPTY && !onBoard) {
            g_dragRack = i; g_dragX = x; g_dragY = y;
            g_dragSlot = raySlotAt(x, y);
            g_dragFromBoard = false;
          }
          break;
        }
      }
      wasDown = down;
      delay(8);
      continue;
    }

    // ── held ─────────────────────────────────────────────────────────────────
    if (down && wasDown) {
      if (abs(x - pressX) > 5 || abs(y - pressY) > 5) moved = true;
      if (g_dragRack >= 0) {
        g_dragX = x; g_dragY = y;
        g_dragSlot = raySlotAt(x, y);      // drives the tray gap preview
        gRenderBoard();
        gRenderLower();
      } else if (moved && g_swipeFrom) {
        // swiping a word: nothing to redraw while the finger travels
      } else if (moved && g_zoom && pressY >= G_BY && pressY < G_BY + G_BSZ) {
        // Drag on the board while zoomed pans the viewport.
        panning = true;
        g_panX = panX0 + (pressX - x);
        g_panY = panY0 + (pressY - y);
        gClampPan();
        gRenderBoard();
      }
      wasDown = down;
      delay(8);
      continue;
    }

    // ── release ──────────────────────────────────────────────────────────────
    if (!down && wasDown) {
      wasDown = down;

      // Finish a tile gesture. A TAP and a DRAG mean different things, and the
      // press handler cannot tell them apart, so it always starts a drag and the
      // distinction is made here:
      //
      //   tap a rack tile      -> select / deselect it (then tap a square to place)
      //   drag a rack tile     -> drop it on a square, or reorder the tray
      //   tap a placed tile    -> return it to the tray (it was lifted at press)
      //   drag a placed tile   -> move it somewhere else
      if (g_dragRack >= 0) {
        int  idx       = g_dragRack;
        int  slot      = g_dragSlot;
        bool fromBoard = g_dragFromBoard;
        g_dragRack = -1;
        g_dragSlot = -1;
        g_dragFromBoard = false;

        if (!moved) {
          // A tap. A lifted board tile simply stays in the tray; a rack tile
          // toggles selection for the tap-to-place route.
          g_selRack = fromBoard ? -1 : ((g_selRack == idx) ? -1 : idx);
        } else {
          uint8_t r, c;
          if (gCellAt(x, y - 10, r, c)) {
            if (gPlaceFrom(idx, r, c)) g_selRack = -1;
          } else if (slot >= 0) {
            g_game.moveRackTile((uint8_t)idx, (uint8_t)slot);
            g_selRack = -1;
          }
        }
        gRenderBoard();
        gRenderLower();
        gRenderNav();
        continue;
      }
      if (panning) { panning = false; continue; }

      // Finish a word swipe: take the word along whichever axis the finger
      // travelled furthest, reading the WHOLE word rather than just the squares
      // actually touched.
      if (moved && g_swipeFrom) {
        g_swipeFrom = false;
        uint8_t er, ec;
        if (gCellAt(x, y, er, ec)) {
          bool horiz = abs((int)ec - (int)g_swipeC) >= abs((int)er - (int)g_swipeR);
          uint8_t w[16];
          uint8_t n = wordAt(g_swipeR, g_swipeC, horiz, w);
          if (!n) n = wordAt(g_swipeR, g_swipeC, !horiz, w);   // try the other axis
          if (n) {
            definitionScreen(w, n);
            tft->fillScreen(COL_BG);
            gRenderAll();
          }
        }
        continue;
      }
      g_swipeFrom = false;
      if (moved) continue;                       // a swipe that grabbed nothing

      // Header back — snapshot to SD so Continue survives a power cycle.
      if (backTapped(x, y)) {
        SD.mkdir(SAVE_DIR);
        g_game.save(SAVE_DIR "/auto.sav");
        gSpritesEnd();
        return;
      }

      // Board tap: place / lift / zoom.
      uint8_t r, c;
      if (gCellAt(x, y, r, c)) {
        if (g_game.isPendingAt(r, c)) {          // lift a tile placed this turn
          g_game.unplaceAt(r, c);
          gRenderBoard(); gRenderLower(); gRenderNav();
        } else if (g_selRack >= 0 && g_game.isEmpty(r, c)) {
          if (gPlaceFrom(g_selRack, r, c)) g_selRack = -1;
          gRenderBoard(); gRenderLower(); gRenderNav();
        } else {
          // Nothing to place: tapping the board toggles zoom, centred here.
          g_zoom = !g_zoom;
          if (g_zoom) gCentreOn(r, c); else { g_panX = g_panY = 0; }
          gRenderBoard();
        }
        continue;
      }

      // Score line under the rack opens the Words-played history.
      {
        int sy = G_LSY + G_HINTH + 2 + G_RT + 2;
        if (y >= sy && y < sy + G_HINTH + 4) {
          wordsPlayedScreen();
          tft->fillScreen(COL_BG);
          gRenderAll();
          continue;
        }
      }

      // Rack tap (no drag): pick up / put down.
      int ry = G_LSY + G_HINTH + 2;
      if (y >= ry && y < ry + G_RT) {
        for (int i = 0; i < RACK_N; i++) {
          if (x >= rackSlotX(i) && x < rackSlotX(i) + G_RT) {
            g_selRack = (g_selRack == i) ? -1 : i;
            gRenderLower();
            break;
          }
        }
        continue;
      }

      // Footer.
      int nh = navHit(x, y);
      if (nh < 0) continue;
      bool placing = g_game.pendingCount() > 0;

      if (nh == 0) {                                   // Master
        bool placed = masterScreen();
        if (placed) { g_zoom = false; g_panX = g_panY = 0; }   // show where it went
        g_selRack = -1;
        tft->fillScreen(COL_BG);
        gRenderAll();
      } else if (placing && nh == 1) {                 // Recall
        g_game.clearPending();
        g_selRack = -1;
        g_zoom = false; g_panX = g_panY = 0;           // zoom out on recall
        gRenderBoard(); gRenderLower(); gRenderNav();
      } else if (placing && nh == 2) {                 // Play
        String bad;
        MoveErr e = g_game.validate(&bad);
        if (e != MV_OK) {
          msgScreen("Invalid move", moveErrText(e),
                    e == MV_BAD_WORD ? bad : String(""), TFT_RED);
          tft->fillScreen(COL_BG);
          gRenderAll();
        } else {
          String w;
          int pts = g_game.scorePending(&w);
          String who = g_game.player(g_game.current()).name;
          g_game.commit();
          g_selRack = -1;
          if (g_game.isOver()) { gSpritesEnd(); gameOver(); return; }
          g_zoom = false; g_panX = g_panY = 0;    // show the whole board again
          g_banner = who + " played " + mainWord(w) + " (+" + pts + ")";
          g_bannerCol = COL_SEL;
          gRenderAll();
        }
      } else if (!placing && nh == 1) {                 // Shuffle
        Player &p = g_game.player(g_game.current());
        for (int i = RACK_N - 1; i > 0; i--) {
          int j = random(i + 1);
          uint8_t t = p.rack[i]; p.rack[i] = p.rack[j]; p.rack[j] = t;
        }
        g_selRack = -1;
        gRenderLower();
      } else if (!placing && nh == 2) {                 // Swap / Skip
        if (swapSkipDialog()) {
          if (g_game.isOver()) { gSpritesEnd(); gameOver(); return; }
          g_banner = "";
        }
        g_selRack = -1;
        tft->fillScreen(COL_BG);
        gRenderAll();
      }
      continue;
    }

    wasDown = down;
    delay(8);
  }
}

// New-game setup: player count, then who is the CPU.
static void newGameFlow() {
  if (!g_dawg.loaded() && !dictLoad(true)) {
    msgScreen("New Game", "Word list not loaded",
              String(LANG_FILES[g_lang]) + " - " + g_dawg.error() +
              ". Copy the .dwg files to " SCRABBLE_DIR " on the SD card.", TFT_RED);
    return;
  }

  static String rows[3] = { "2 Players", "3 Players", "4 Players" };
  int n = scrollList("New Game", rows, 3, true);
  if (n < 0) return;
  uint8_t players = n + 2;

  static String orows[2] = { "All human (hotseat)", "Player 2 is the CPU" };
  int mode = scrollList("Opponent", orows, 2, true);
  if (mode < 0) return;

  uint8_t level = 1;
  if (mode == 1) {
    static String drows[3] = { "Easy", "Normal", "Hard" };
    int d = scrollList("Difficulty", drows, 3, true);
    if (d < 0) return;
    level = (uint8_t)d;
  }

  // Must precede begin(): that is what fills, spreads and deals the bag.
  g_game.setLuckHelper(g_luckHelper);
  g_game.begin(&g_dawg, g_lang, players);
  g_game.setCrossLimit(g_crossLimit);
  for (uint8_t p = 0; p < players; p++) {
    char nm[12];
    // Player 1 is whoever is holding the device.
    if (p == 0) strcpy(nm, "You");
    else        snprintf(nm, sizeof(nm), "P%u", (unsigned)(p + 1));
    g_game.setPlayer(p, nm, false);
  }
  if (mode == 1) g_game.setPlayer(1, "CPU", true, level);
  g_gameActive = true;

  gameScreen();
}

// Main menu (H4W9-style large rounded buttons)
static const char *MENU_ITEMS[] = { "New Game", "Continue", "Board Builder",
                                    "Dictionary Editor", "Settings" };
static const int    MENU_COUNT  = 5;
static const int    MENU_MARGIN = 16;
static const int    MENU_TOP    = CONTENTY + 12;
static const int    MENU_GAP    = 12;
static int menuBtnH() {
  int avail = SCRH - MENU_TOP - 12;
  return (avail - (MENU_COUNT - 1) * MENU_GAP) / MENU_COUNT;
}
static int menuBtnY(int i) { return MENU_TOP + i * (menuBtnH() + MENU_GAP); }
static int menuButtonAt(uint16_t x, uint16_t y) {
  if ((int)x < MENU_MARGIN || (int)x >= SCRW - MENU_MARGIN) return -1;
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int by = menuBtnY(i);
    if ((int)y >= by && (int)y < by + bh) return i;
  }
  return -1;
}

static void drawMenu() {
  tft->fillScreen(COL_BG);
  drawHeader(FW_NAME, false);
  int bh = menuBtnH();
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = menuBtnY(i);
    tft->fillRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, COL_ACCENT);
    tft->drawRoundRect(MENU_MARGIN, y, SCRW - 2 * MENU_MARGIN, bh, 12, theme.neon(i * 3, COL_DIM));
    tft->setTextColor(COL_FG, COL_ACCENT);
    tft->setTextDatum(MC_DATUM);
#ifdef MARAUDER_V8
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, 2);
#else
    // "Dictionary Editor" is wider than a button at font 4; drop a size rather
    // than let it run under the rounded corners.
    int f = (strWidth(MENU_ITEMS[i], 4) <= SCRW - 2 * MENU_MARGIN - 16) ? 4 : 2;
    drawStr(MENU_ITEMS[i], SCRW / 2, y + bh / 2, f);
#endif
  }
  tft->setTextDatum(TL_DATUM);
}

static void openMenuItem(int i) {
  switch (i) {
    case 0: newGameFlow(); break;
    case 1: {                                   // Continue
      if (g_gameActive) { gameScreen(); break; }
      // Nothing in memory — try the snapshot written when the board was left.
      if (!g_dawg.loaded() && !dictLoad(true)) {
        msgScreen("Continue", "Word list not loaded", g_dawg.error(), TFT_RED);
        break;
      }
      if (g_game.load(SAVE_DIR "/auto.sav", &g_dawg)) {
        g_game.setCrossLimit(g_crossLimit);
        g_game.setLuckHelper(g_luckHelper);
        // A save from the other language would be scored against the wrong bag.
        if (g_game.lang() != g_lang) {
          msgScreen("Continue", "Saved game is in the other language",
                    String("Switch Language back to ") +
                    LANG_NAMES[g_game.lang()] + " in Settings to resume it.", TFT_RED);
          break;
        }
        g_gameActive = true;
        gameScreen();
      } else {
        msgScreen("Continue", "No saved game",
                  "Start a new game. It is saved automatically when you leave "
                  "the board.", COL_DIM);
      }
      break;
    }
    case 2: boardBuilderScreen(); break;
    case 3: dictionaryScreen();   break;
    case 4: settingsFlow();       break;
    default: break;
  }
  drawMenu();
}

static bool mainMenuStart(ViewManager *viewManager) {
  drawMenu();
  return true;
}

static void mainMenuRun(ViewManager *viewManager) {
  static bool wasDown = false;
  TouchInput *t = viewManager->getInputManager()->getTouch();
  bool down = t->isPressed();
  if (down && !wasDown) {                              // fresh tap (press edge)
    uint16_t x = t->x(), y = t->y();
    int btn = menuButtonAt(x, y);
    if (btn >= 0) openMenuItem(btn);
  }
  wasDown = down;

  // Idle refresh of ONLY the header status corner + memory readout. The menu
  // buttons don't depend on either, so never repaint the whole screen here —
  // doing so made the menu flash repeatedly while a background connect cycled.
  static uint32_t lastRefresh = 0;
  static int lastStatus = -2;
  static bool lastConn = false;
  if (WiFi.status() != lastStatus || g_wifiConnecting != lastConn || millis() - lastRefresh > 4000) {
    lastRefresh = millis();
    lastStatus  = WiFi.status();
    lastConn    = g_wifiConnecting;
    drawHeaderStatus();
    drawHeaderMem(false);
  }
}

static const PROGMEM View mainMenuView = View("MainMenu", mainMenuRun, mainMenuStart, nullptr);

// Arduino entry points
void setup() {
  randomSeed(esp_random());
#ifndef DEVELOPER
  esp_log_level_set("*", ESP_LOG_NONE);
#endif

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);
  Serial.println(F("[" BOARD_NAME "] Scrabble starting..."));

  // Backlight off during init (PWM).
  pinMode(TFT_BL, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 0);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);
#endif

  // SD (shared FSPI bus on ESP32-C5) — must be up before ViewManager (Storage).
#ifdef HAS_C5_SD
  sharedSPI.begin(SD_SCK, SD_MISO, SD_MOSI);
  delay(100);
  if (!SD.begin(SD_CS, sharedSPI)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
  else Serial.println(F("[" BOARD_NAME "] SD OK"));
#else
  if (!SD.begin(SD_CS)) Serial.println(F("[" BOARD_NAME "] SD init failed"));
#endif

  // SPIFFS for settings (format on first boot).
  if (!SPIFFS.begin(true)) Serial.println(F("[" BOARD_NAME "] SPIFFS mount failed"));
  else                     Serial.println(F("[" BOARD_NAME "] SPIFFS OK"));

#ifdef HAS_PSRAM
  // The word lists are ~2 MB and live in PSRAM. Without it the Dictionary screen
  // reports "out of memory" — check Tools -> PSRAM -> Enabled.
  if (!psramInit()) Serial.println(F("[" BOARD_NAME "] PSRAM unavailable"));
#endif

#ifdef HAS_CAP_TOUCH
  // Capacitive touch (also does Wire.begin on the shared I2C bus).
  ft6336_init();
#else
  // V8 has no I2C touch controller (XPT2046 rides the SPI bus), but the fuel
  // gauge below still needs the I2C bus that ft6336_init() would have opened.
  Wire.begin(I2C_SDA, I2C_SCL, 400000U);
#endif
  battInit();                          // MAX17048 fuel gauge on the same I2C bus

  // Load persisted settings before anything draws.
  theme.load();
  cfgLoad();
  // Both of these read the SD card, so they belong after it is mounted and
  // after cfgLoad() has said which board to use.
  gridBegin(g_gridName);   // premium-square layout (Board Builder)
  weBegin();               // Dictionary Editor overrides
  g_dist.begin();          // editable letter distribution (Settings -> Letters)

  // Put the status LED in a known-off state (also arms the V8's PWM channel).
  ledOff();

  // ViewManager owns the panel (Draw) and touch (InputManager).
#ifdef MARAUDER_V8
  vm    = new ViewManager(MarauderV8Config);
#else
  vm    = new ViewManager(PancakeConfig);
#endif
  tft   = vm->getDraw()->display->getTFT();
  touch = vm->getInputManager()->getTouch();
  applyThemeToViewManager();

  // Backlight on at the saved brightness.
  applyBrightness();

#ifndef HAS_CAP_TOUCH
  // Resistive panel: point TouchInput at TFT_eSPI's XPT2046 reader, then load
  // the stored calibration (or run the wizard). Must come after the backlight is
  // up, or a first-boot user would be tapping an unlit screen.
  if (touch) touch->attachTFT(tft);
  touchCalInit();
#endif

  // WiFi: capture disconnect reasons for diagnostics.
  WiFi.onEvent(wifiEvent);
  WiFi.mode(WIFI_STA);

  // Show the main menu immediately, then connect to saved WiFi in the background
  // (header icon + LED report progress). The word list is NOT loaded here — it is
  // a ~2 MB SD read, and making the menu wait on it would be a poor first impression.
  vm->add(&mainMenuView);
  vm->set("MainMenu");
  wifiBgBegin();
  drawHeaderStatus();                  // show the "connecting" (yellow) icon at once

  Serial.println(F("[" BOARD_NAME "] Ready."));
}

void loop() {
  vm->run();
  wifiBgTick();                        // advance the background WiFi connect

  // Reconnect watchdog: ONLY on the drop edge (connected -> lost), make one
  // reconnect pass (the two closest saved nets). If it fails we stay disconnected
  // rather than retrying forever — the LED goes off instead of pulsing amber.
  static bool wasConnected = false;
  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (wasConnected && !nowConnected && !g_manualDisconnect && (g_wb == WB_DONE || g_wb == WB_IDLE)) {
    wifiBgBegin();
  }
  wasConnected = nowConnected;

  // Activity LED mirrors the connecting state (on while scanning/associating).
  static bool ledState = false;
  if (g_wifiConnecting != ledState) { ledState = g_wifiConnecting; ledSet(ledState); }

  delay(5);
}
