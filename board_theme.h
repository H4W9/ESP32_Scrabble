// board_theme.h
// Board + tile appearance for ESP32 Scrabble.
//
// The default look, "Classic", is a dark board with jewel-tone premium squares.
// Every colour below is a fixed RGB565 value, and the default premium-square
// layout is completed through the board's 8-fold symmetry.
//
// Its premium distribution (8 TW, 20 TL, 12 DW + centre, 24 DL) is deliberately
// NOT the standard Scrabble layout -- Scrabble has 8 TW, 12 TL, 16 DW + centre,
// 24 DL in different positions.

#pragma once
#include <Arduino.h>

// Premium square kinds. The char codes are the on-disk grid format (boardgrid.h).
enum Premium : uint8_t { PR_NONE = 0, PR_DL, PR_TL, PR_DW, PR_TW, PR_CENTRE };

// The layout itself is no longer here: it is editable now, and lives as runtime
// data in boardgrid.cpp (Board Builder). g_grid holds one Premium per square,
// row-major. Classic — the layout this file used to hard-code — is still the
// default and is kept there in its character form.
extern uint8_t g_grid[225];

static inline Premium premiumAt(uint8_t row, uint8_t col) {
  if (row > 14 || col > 14) return PR_NONE;
  uint8_t p = g_grid[row * 15 + col];
  return (p <= PR_CENTRE) ? (Premium)p : PR_NONE;
}

// Score multipliers. The centre star doubles the word like any DW.
static inline uint8_t letterMult(Premium p) {
  return p == PR_DL ? 2 : p == PR_TL ? 3 : 1;
}
static inline uint8_t wordMult(Premium p) {
  return (p == PR_DW || p == PR_CENTRE) ? 2 : p == PR_TW ? 3 : 1;
}

// Fixed board palette (RGB565), one colour per cell/tile role.
struct BoardPal {
  uint16_t gutter;      // board background, shows through as the grid lines
  uint16_t empty;       // plain playable square
  uint16_t dl, tl, dw, tw;
  uint16_t prem_text;   // the "DL"/"TW" labels
  uint16_t tile;        // placed tile face
  uint16_t tile_last;   // tiles from the most recent move
  uint16_t tile_hold;   // tiles placed this turn, not yet submitted
  uint16_t tile_edge;   // tile bevel / shadow
  uint16_t tile_text;   // letter
  uint16_t tile_val;    // the small letter-value superscript
  const char *name;
};

static const BoardPal BOARD_CLASSIC = {
    /* gutter    */ 0x10C3,   // #141A1C
    /* empty     */ 0x2946,   // #282830
    /* dl        */ 0x6C4A,   // #688850  green
    /* tl        */ 0x1B12,   // #186090  blue
    /* dw        */ 0xCB81,   // #C87008  orange
    /* tw        */ 0x9145,   // #902828  dark red
    /* prem_text */ 0xFFFF,
    /* tile      */ 0xF75D,   // #F0E8E8  warm off-white
    /* tile_last */ 0xFF94,   // #F8F0A0  pale yellow
    /* tile_hold */ 0xCE9B,   // #C8D0D8  cool grey-blue
    /* tile_edge */ 0xCE17,   // #C8C0B8
    /* tile_text */ 0x0000,
    /* tile_val  */ 0x0000,
    /* name      */ "Classic",
};

// A board palette derived from whichever UI theme is active, for users who
// would rather the board follow the rest of the shell. Kept close to the
// default palette's structure -- the premium colours stay, only the neutrals
// (gutter, empty square, tile face) move to the theme, because the premium
// colours are the part that carries meaning.
static inline BoardPal boardPalFromTheme(uint16_t bg, uint16_t fg, uint16_t dim, bool dark) {
  BoardPal p = BOARD_CLASSIC;
  p.gutter    = bg;
  p.empty     = dim;
  p.tile      = dark ? 0xF75D : 0xFFFF;
  p.tile_text = dark ? 0x0000 : 0x0000;
  p.tile_val  = 0x0000;
  p.tile_edge = dark ? 0xCE17 : 0xAD55;
  p.prem_text = fg;
  p.name      = "Match Theme";
  return p;
}

static const uint8_t BOARD_PAL_CLASSIC = 0;
static const uint8_t BOARD_PAL_THEME    = 1;
static const uint8_t BOARD_PAL_COUNT    = 2;
static const char *const BOARD_PAL_NAMES[BOARD_PAL_COUNT] = { "Classic", "Match Theme" };
