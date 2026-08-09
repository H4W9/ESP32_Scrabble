// letters.h
// Editable letter distribution — the point value and bag count of every tile,
// per language, editable from the Letter Distribution screen.
//
// This used to be a pair of `static const` tables inside game.cpp. It is a
// separate module now because it is really user configuration, not engine data:
// it outlives any one Game, it is edited from Settings, and it persists.
//
// The built-in defaults are the sets documented in game.cpp — a ~3x-scale
// English table (A/E/I/O = 3, not 1) and the official German Scrabble SDeV set.
// Editing is per language and is saved to SPIFFS, so a change to English never
// disturbs German.
//
// Letters use the firmware's letter bytes: 'A'..'Z', 0x80 Ae, 0x82 Oe, 0x84 Ue,
// and TILE_BLANK (1) for the blanks. There is deliberately no eszett — see
// game.h.

#pragma once
#include <Arduino.h>

#define DIST_MAX_ENTRIES 32     // 26 letters + 3 umlauts + blank, with headroom
#define DIST_MAX_VALUE   99
#define DIST_MAX_COUNT   30
#define DIST_MAX_TILES  104     // must not exceed MAX_BAG in game.h
#define DIST_MAX_BINGO  500
#define DIST_BINGO_STEP   5     // steppers move in 5s; 1s would be tedious here

// Bonus for laying all seven rack tiles in one move, per language. Scrabble uses
// 50. The default English letters run ~3x Scrabble's, so 50 may well be far too
// small there -- which is exactly why this is editable rather than compiled in.
#define DIST_DEFAULT_BINGO 50

struct TileDef { uint8_t letter, value, count; };

class LetterDist {
public:
  // Load both languages from SPIFFS, falling back to the built-in defaults.
  void begin();

  uint8_t  count(uint8_t lang) const { return _n[lang & 1]; }
  TileDef &at(uint8_t lang, uint8_t i) { return _d[lang & 1][i]; }
  const TileDef &at(uint8_t lang, uint8_t i) const { return _d[lang & 1][i]; }

  uint8_t  valueOf(uint8_t lang, uint8_t letter) const;
  uint16_t totalTiles(uint8_t lang) const;

  uint16_t bingo(uint8_t lang) const { return _bingo[lang & 1]; }
  void     setBingo(uint8_t lang, uint16_t v) {
    _bingo[lang & 1] = (v > DIST_MAX_BINGO) ? DIST_MAX_BINGO : v;
  }

  // True when the table differs from the built-in default for that language.
  bool edited(uint8_t lang) const;

  void reset(uint8_t lang);
  bool save(uint8_t lang) const;
  bool load(uint8_t lang);

private:
  TileDef  _d[2][DIST_MAX_ENTRIES];
  uint8_t  _n[2] = { 0, 0 };
  uint16_t _bingo[2] = { DIST_DEFAULT_BINGO, DIST_DEFAULT_BINGO };
  void loadDefaults(uint8_t lang);
};

extern LetterDist g_dist;
