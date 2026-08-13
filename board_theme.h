// board_theme.h
// Board + tile appearance for ESP32 Scrabble.
//
// The default look, "Classic", is a dark board with jewel-tone premium squares.
// Beyond it there is now a gallery of fixed palettes (Settings -> Grid Look),
// exposed through the same small interface theme.h expects (BoardPal, BOARD_PALS,
// boardPalFromTheme, BOARD_PAL_*). The Grid Look choice is a separate setting from
// the UI theme, so a light board can sit under a dark shell and vice-versa; the
// final entry, "Match Theme", is computed from the active UI theme rather than
// being a fixed table row.
//
// Across every palette the four premium hues stay saturated enough for the white
// "DL"/"TW" labels to read, because those colours are the part that carries game
// meaning (letter/word multipliers). What changes between palettes is the board
// neutrals (gutter lines, empty squares) and the tile faces.
//
// The default premium *distribution* (8 TW, 20 TL, 12 DW + centre, 24 DL) is
// deliberately NOT the standard Scrabble layout, and it is editable now: it lives
// as runtime data in boardgrid.cpp (Board Builder), not here.

#pragma once
#include <Arduino.h>

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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

// ── Classic — dark board, jewel-tone premiums, warm off-white tiles. ─────────
static const BoardPal BOARD_CLASSIC = {
    /* gutter    */ rgb565(0x14, 0x1A, 0x1C),
    /* empty     */ rgb565(0x28, 0x28, 0x30),
    /* dl        */ rgb565(0x68, 0x88, 0x50),
    /* tl        */ rgb565(0x18, 0x60, 0x90),
    /* dw        */ rgb565(0xC8, 0x70, 0x08),
    /* tw        */ rgb565(0x90, 0x28, 0x28),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF0, 0xE8, 0xE8),
    /* tile_last */ rgb565(0xF8, 0xF0, 0xA0),
    /* tile_hold */ rgb565(0xC8, 0xD0, 0xD8),
    /* tile_edge */ rgb565(0xC8, 0xC0, 0xB8),
    /* tile_text */ rgb565(0x00, 0x00, 0x00),
    /* tile_val  */ rgb565(0x00, 0x00, 0x00),
    /* name      */ "Classic",
};

// ── Dark — near-black board, bright premiums, white tiles. ───────────────────
static const BoardPal BOARD_DARK = {
    /* gutter    */ rgb565(0x00, 0x00, 0x00),
    /* empty     */ rgb565(0x18, 0x18, 0x1E),
    /* dl        */ rgb565(0x4E, 0x8A, 0x54),
    /* tl        */ rgb565(0x2E, 0x6A, 0xA8),
    /* dw        */ rgb565(0xC8, 0x7A, 0x18),
    /* tw        */ rgb565(0xA8, 0x36, 0x36),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF4, 0xF4, 0xF4),
    /* tile_last */ rgb565(0xF6, 0xEE, 0x9A),
    /* tile_hold */ rgb565(0xC6, 0xD0, 0xDA),
    /* tile_edge */ rgb565(0xB4, 0xB4, 0xB4),
    /* tile_text */ rgb565(0x00, 0x00, 0x00),
    /* tile_val  */ rgb565(0x00, 0x00, 0x00),
    /* name      */ "Dark",
};

// ── Grayscale — light board, premiums as distinct greys. ─────────────────────
static const BoardPal BOARD_GRAY = {
    /* gutter    */ rgb565(0x9A, 0x9A, 0x9A),
    /* empty     */ rgb565(0xEC, 0xEC, 0xEC),
    /* dl        */ rgb565(0x70, 0x70, 0x70),
    /* tl        */ rgb565(0x58, 0x58, 0x58),
    /* dw        */ rgb565(0x88, 0x88, 0x88),
    /* tw        */ rgb565(0x44, 0x44, 0x44),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xFB, 0xFB, 0xFB),
    /* tile_last */ rgb565(0xDE, 0xDE, 0xC4),
    /* tile_hold */ rgb565(0xCE, 0xD2, 0xD6),
    /* tile_edge */ rgb565(0xB0, 0xB0, 0xB0),
    /* tile_text */ rgb565(0x14, 0x14, 0x14),
    /* tile_val  */ rgb565(0x14, 0x14, 0x14),
    /* name      */ "Grayscale",
};

// ── Midnight — deep indigo board, jewel accents. ─────────────────────────────
static const BoardPal BOARD_MIDNIGHT = {
    /* gutter    */ rgb565(0x0E, 0x16, 0x30),
    /* empty     */ rgb565(0x17, 0x21, 0x42),
    /* dl        */ rgb565(0x3E, 0x84, 0x5E),
    /* tl        */ rgb565(0x3A, 0x5A, 0xB8),
    /* dw        */ rgb565(0xC0, 0x8A, 0x2E),
    /* tw        */ rgb565(0xB0, 0x4E, 0x7A),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xE8, 0xEC, 0xFF),
    /* tile_last */ rgb565(0xF2, 0xE6, 0xB0),
    /* tile_hold */ rgb565(0xB8, 0xC2, 0xE0),
    /* tile_edge */ rgb565(0x2A, 0x3A, 0x66),
    /* tile_text */ rgb565(0x12, 0x18, 0x3A),
    /* tile_val  */ rgb565(0x12, 0x18, 0x3A),
    /* name      */ "Midnight",
};

// ── Sepia — warm cream board, the printed-set look. ──────────────────────────
static const BoardPal BOARD_SEPIA = {
    /* gutter    */ rgb565(0x6A, 0x56, 0x40),
    /* empty     */ rgb565(0xE7, 0xD8, 0xBE),
    /* dl        */ rgb565(0x7C, 0x8B, 0x4E),
    /* tl        */ rgb565(0x4E, 0x78, 0x96),
    /* dw        */ rgb565(0xC0, 0x8A, 0x3E),
    /* tw        */ rgb565(0xA8, 0x54, 0x36),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF7, 0xEF, 0xDD),
    /* tile_last */ rgb565(0xE9, 0xD0, 0x8A),
    /* tile_hold */ rgb565(0xD8, 0xC6, 0xA8),
    /* tile_edge */ rgb565(0xC9, 0xB4, 0x8F),
    /* tile_text */ rgb565(0x3A, 0x2E, 0x1E),
    /* tile_val  */ rgb565(0x3A, 0x2E, 0x1E),
    /* name      */ "Sepia",
};

// ── Forest — soft green paper board. ─────────────────────────────────────────
static const BoardPal BOARD_FOREST = {
    /* gutter    */ rgb565(0x2E, 0x4A, 0x28),
    /* empty     */ rgb565(0xDD, 0xE9, 0xD4),
    /* dl        */ rgb565(0x3E, 0x7D, 0x42),
    /* tl        */ rgb565(0x2E, 0x76, 0x80),
    /* dw        */ rgb565(0xC0, 0x8A, 0x2E),
    /* tw        */ rgb565(0xB0, 0x38, 0x38),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF1, 0xF7, 0xEE),
    /* tile_last */ rgb565(0xE6, 0xE0, 0xA0),
    /* tile_hold */ rgb565(0xC8, 0xD6, 0xBE),
    /* tile_edge */ rgb565(0xB4, 0xC9, 0xA6),
    /* tile_text */ rgb565(0x1E, 0x2E, 0x19),
    /* tile_val  */ rgb565(0x1E, 0x2E, 0x19),
    /* name      */ "Forest",
};

// ── Ocean — cool teal paper board. ───────────────────────────────────────────
static const BoardPal BOARD_OCEAN = {
    /* gutter    */ rgb565(0x0F, 0x3D, 0x47),
    /* empty     */ rgb565(0xD3, 0xEC, 0xEF),
    /* dl        */ rgb565(0x2E, 0x9E, 0x8E),
    /* tl        */ rgb565(0x1E, 0x7C, 0x9E),
    /* dw        */ rgb565(0xC0, 0x8A, 0x2E),
    /* tw        */ rgb565(0xC2, 0x4E, 0x4E),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF0, 0xFA, 0xFB),
    /* tile_last */ rgb565(0xE6, 0xE6, 0xA0),
    /* tile_hold */ rgb565(0xBE, 0xDD, 0xE2),
    /* tile_edge */ rgb565(0xA6, 0xCD, 0xD4),
    /* tile_text */ rgb565(0x0E, 0x2A, 0x30),
    /* tile_val  */ rgb565(0x0E, 0x2A, 0x30),
    /* name      */ "Ocean",
};

// ── Rose — warm pink paper board. ────────────────────────────────────────────
static const BoardPal BOARD_ROSE = {
    /* gutter    */ rgb565(0x5A, 0x24, 0x36),
    /* empty     */ rgb565(0xF4, 0xD9, 0xE2),
    /* dl        */ rgb565(0x5E, 0x9E, 0x5E),
    /* tl        */ rgb565(0x4E, 0x7A, 0xC2),
    /* dw        */ rgb565(0xD2, 0x69, 0x1E),
    /* tw        */ rgb565(0xC2, 0x18, 0x5B),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xFE, 0xF3, 0xF6),
    /* tile_last */ rgb565(0xF0, 0xD6, 0xA0),
    /* tile_hold */ rgb565(0xE8, 0xC6, 0xD3),
    /* tile_edge */ rgb565(0xE0, 0xAE, 0xC0),
    /* tile_text */ rgb565(0x3A, 0x1E, 0x28),
    /* tile_val  */ rgb565(0x3A, 0x1E, 0x28),
    /* name      */ "Rose",
};

// ── Sunset — cream board, amber accents. ─────────────────────────────────────
static const BoardPal BOARD_SUNSET = {
    /* gutter    */ rgb565(0x6A, 0x3E, 0x1E),
    /* empty     */ rgb565(0xFB, 0xE6, 0xC8),
    /* dl        */ rgb565(0x7A, 0x9E, 0x3E),
    /* tl        */ rgb565(0x4E, 0x78, 0x96),
    /* dw        */ rgb565(0xD2, 0x69, 0x1E),
    /* tw        */ rgb565(0xB8, 0x32, 0x20),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xFF, 0xF6, 0xE9),
    /* tile_last */ rgb565(0xFB, 0xD6, 0x9A),
    /* tile_hold */ rgb565(0xEA, 0xD3, 0xB0),
    /* tile_edge */ rgb565(0xE6, 0xC7, 0x9A),
    /* tile_text */ rgb565(0x3A, 0x2A, 0x18),
    /* tile_val  */ rgb565(0x3A, 0x2A, 0x18),
    /* name      */ "Sunset",
};

// ── Lavender — pale purple paper board. ──────────────────────────────────────
static const BoardPal BOARD_LAVENDER = {
    /* gutter    */ rgb565(0x3E, 0x2A, 0x5A),
    /* empty     */ rgb565(0xE4, 0xDA, 0xF3),
    /* dl        */ rgb565(0x5E, 0x9E, 0x5E),
    /* tl        */ rgb565(0x6A, 0x5A, 0xD0),
    /* dw        */ rgb565(0xC0, 0x8A, 0x2E),
    /* tw        */ rgb565(0xB0, 0x30, 0x50),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF6, 0xF2, 0xFC),
    /* tile_last */ rgb565(0xE6, 0xD6, 0xA0),
    /* tile_hold */ rgb565(0xD0, 0xBC, 0xF2),
    /* tile_edge */ rgb565(0xC4, 0xB4, 0xE0),
    /* tile_text */ rgb565(0x28, 0x1E, 0x3A),
    /* tile_val  */ rgb565(0x28, 0x1E, 0x3A),
    /* name      */ "Lavender",
};

// ── Mint — fresh green-cyan paper board. ─────────────────────────────────────
static const BoardPal BOARD_MINT = {
    /* gutter    */ rgb565(0x17, 0x40, 0x2E),
    /* empty     */ rgb565(0xD2, 0xEE, 0xE0),
    /* dl        */ rgb565(0x2E, 0x9E, 0x6E),
    /* tl        */ rgb565(0x1E, 0x8E, 0x9E),
    /* dw        */ rgb565(0xC0, 0x8A, 0x2E),
    /* tw        */ rgb565(0xC2, 0x28, 0x28),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xF0, 0xFB, 0xF6),
    /* tile_last */ rgb565(0xE6, 0xE6, 0xA0),
    /* tile_hold */ rgb565(0xBE, 0xE0, 0xCE),
    /* tile_edge */ rgb565(0xA6, 0xD6, 0xC2),
    /* tile_text */ rgb565(0x12, 0x30, 0x2A),
    /* tile_val  */ rgb565(0x12, 0x30, 0x2A),
    /* name      */ "Mint",
};

// ── Slate — muted blue-grey dark board. ──────────────────────────────────────
static const BoardPal BOARD_SLATE = {
    /* gutter    */ rgb565(0x14, 0x1C, 0x26),
    /* empty     */ rgb565(0x26, 0x31, 0x3E),
    /* dl        */ rgb565(0x4E, 0x8A, 0x6A),
    /* tl        */ rgb565(0x3A, 0x6A, 0x9E),
    /* dw        */ rgb565(0xB8, 0x8A, 0x3E),
    /* tw        */ rgb565(0xA8, 0x54, 0x54),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xE6, 0xEC, 0xF2),
    /* tile_last */ rgb565(0xEE, 0xE6, 0xA0),
    /* tile_hold */ rgb565(0xB8, 0xC2, 0xCE),
    /* tile_edge */ rgb565(0x38, 0x46, 0x56),
    /* tile_text */ rgb565(0x14, 0x20, 0x2C),
    /* tile_val  */ rgb565(0x14, 0x20, 0x2C),
    /* name      */ "Slate",
};

// ── Carbon — near-black board, dark tiles with white letters (night look). ───
static const BoardPal BOARD_CARBON = {
    /* gutter    */ rgb565(0x06, 0x06, 0x06),
    /* empty     */ rgb565(0x16, 0x16, 0x16),
    /* dl        */ rgb565(0x2E, 0x6A, 0x3E),
    /* tl        */ rgb565(0x2E, 0x5A, 0x8A),
    /* dw        */ rgb565(0xA8, 0x7A, 0x2E),
    /* tw        */ rgb565(0xA8, 0x3E, 0x3E),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0x24, 0x24, 0x24),
    /* tile_last */ rgb565(0x4A, 0x46, 0x1E),
    /* tile_hold */ rgb565(0x2A, 0x34, 0x40),
    /* tile_edge */ rgb565(0x00, 0x00, 0x00),
    /* tile_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile_val  */ rgb565(0xFF, 0xFF, 0xFF),
    /* name      */ "Carbon",
};

// ── Contrast — accessibility: white board, black grid, bold premiums. ────────
static const BoardPal BOARD_CONTRAST = {
    /* gutter    */ rgb565(0x00, 0x00, 0x00),
    /* empty     */ rgb565(0xFF, 0xFF, 0xFF),
    /* dl        */ rgb565(0x00, 0xA8, 0x3E),
    /* tl        */ rgb565(0x00, 0x44, 0xCC),
    /* dw        */ rgb565(0xFF, 0x8A, 0x00),
    /* tw        */ rgb565(0xE0, 0x00, 0x00),
    /* prem_text */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile      */ rgb565(0xFF, 0xFF, 0xFF),
    /* tile_last */ rgb565(0xFF, 0xE2, 0x4D),
    /* tile_hold */ rgb565(0xC8, 0xE0, 0xFF),
    /* tile_edge */ rgb565(0x00, 0x00, 0x00),
    /* tile_text */ rgb565(0x00, 0x00, 0x00),
    /* tile_val  */ rgb565(0x00, 0x00, 0x00),
    /* name      */ "Contrast",
};

// ── Terminal — retro green phosphor on black; dark tiles, green letters. ──────
static const BoardPal BOARD_TERMINAL = {
    /* gutter    */ rgb565(0x04, 0x10, 0x08),
    /* empty     */ rgb565(0x0A, 0x1C, 0x10),
    /* dl        */ rgb565(0x1E, 0x7A, 0x40),
    /* tl        */ rgb565(0x16, 0x60, 0x3A),
    /* dw        */ rgb565(0x2E, 0x9A, 0x50),
    /* tw        */ rgb565(0x3E, 0xAA, 0x30),
    /* prem_text */ rgb565(0x8C, 0xFF, 0xC0),
    /* tile      */ rgb565(0x0A, 0x22, 0x14),
    /* tile_last */ rgb565(0x18, 0x44, 0x26),
    /* tile_hold */ rgb565(0x10, 0x30, 0x1E),
    /* tile_edge */ rgb565(0x10, 0x3A, 0x20),
    /* tile_text */ rgb565(0x40, 0xE0, 0x80),
    /* tile_val  */ rgb565(0x40, 0xE0, 0x80),
    /* name      */ "Terminal",
};

// Fixed palettes, in cycle order. "Match Theme" is appended as the last option
// but is computed (below), not stored here.
static const BoardPal BOARD_PALS[] = {
    BOARD_CLASSIC, BOARD_DARK,     BOARD_GRAY,   BOARD_MIDNIGHT, BOARD_SEPIA,
    BOARD_FOREST,  BOARD_OCEAN,    BOARD_ROSE,   BOARD_SUNSET,   BOARD_LAVENDER,
    BOARD_MINT,    BOARD_SLATE,    BOARD_CARBON, BOARD_CONTRAST, BOARD_TERMINAL,
};
static const uint8_t BOARD_PAL_FIXED = sizeof(BOARD_PALS) / sizeof(BOARD_PALS[0]);

// A board palette derived from whichever UI theme is active, for users who would
// rather the board follow the rest of the shell. Kept close to Classic's
// structure -- the premium colours stay (they carry meaning), only the neutrals
// (gutter, empty square, tile face) move to the theme.
static inline BoardPal boardPalFromTheme(uint16_t bg, uint16_t fg, uint16_t dim, bool dark) {
  BoardPal p = BOARD_CLASSIC;
  p.gutter    = bg;
  p.empty     = dim;
  p.tile      = dark ? rgb565(0xF0, 0xE8, 0xE8) : rgb565(0xFF, 0xFF, 0xFF);
  p.tile_text = rgb565(0x00, 0x00, 0x00);
  p.tile_val  = rgb565(0x00, 0x00, 0x00);
  p.tile_edge = dark ? rgb565(0xC8, 0xC0, 0xB8) : rgb565(0xAD, 0xAD, 0xAD);
  p.prem_text = fg;
  p.name      = "Match Theme";
  return p;
}

static const uint8_t BOARD_PAL_CLASSIC = 0;
static const uint8_t BOARD_PAL_COUNT   = BOARD_PAL_FIXED + 1;   // + Match Theme
static const uint8_t BOARD_PAL_THEME   = BOARD_PAL_COUNT - 1;   // last = computed
static const char *const BOARD_PAL_NAMES[BOARD_PAL_COUNT] = {
    "Classic", "Dark",  "Grayscale", "Midnight", "Sepia",
    "Forest",  "Ocean", "Rose",      "Sunset",   "Lavender",
    "Mint",    "Slate", "Carbon",    "Contrast", "Terminal",
    "Match Theme",
};
static_assert(sizeof(BOARD_PAL_NAMES) / sizeof(BOARD_PAL_NAMES[0]) == BOARD_PAL_COUNT,
              "BOARD_PAL_NAMES must have one entry per palette (fixed + Match Theme)");
