// boardgrid.h
// The premium-square layout, as editable data rather than a compiled-in table.
//
// The board is one Premium per square with a name, serialised to a fixed-length
// string; there is a set of built-ins plus a random generator, and a game can
// carry its own custom layout. The Board Builder screen edits it by tapping a
// cell to cycle its bonus.
//
// SYMMETRY is the thing that makes a board playable rather than merely legal.
// The layout is symmetric under both mirrors and the transpose, and the
// transpose part matters to this firmware specifically: an asymmetric board
// hands one axis more scoring potential than the other, and the opponent — which
// searches both axes evenly — would then look biased for a reason that is really
// the board's fault. The editor paints the whole 8-cell orbit at once.

#pragma once
#include <Arduino.h>

#define GRID_N        15
#define GRID_CELLS    (GRID_N * GRID_N)
#define GRID_NAME_MAX 16
#define GRID_MAX_SAVED 12

// The layout the game is currently played on. Values are Premium (board_theme.h);
// stored as raw bytes here so this header doesn't have to include that one.
extern uint8_t g_grid[GRID_CELLS];
extern char    g_gridName[GRID_NAME_MAX];

// Load the layout named in config, falling back to the built-in Classic.
void gridBegin(const char *name);

// Built-in layouts. 0 is always Classic, the default board.
uint8_t     gridBuiltinCount();
const char *gridBuiltinName(uint8_t i);
bool        gridLoadBuiltin(uint8_t i);

// A fresh symmetric layout with the same premium budget as Classic.
void gridGenerateRandom();

// How many of each Premium kind the current grid holds, indexed by Premium.
void gridCounts(uint8_t out[6]);

// Paint a cell and its whole symmetry orbit (both mirrors + transpose).
void gridPaint(uint8_t row, uint8_t col, uint8_t premium);
// The bonus a tap cycles to next: none -> DL -> TL -> DW -> TW -> none.
uint8_t gridNextBonus(uint8_t premium);

// Saved layouts, one file each under /scrabble/grids/.
uint8_t gridListSaved(String *out, uint8_t maxN);
bool    gridSave(const char *name);
bool    gridLoad(const char *name);
bool    gridDelete(const char *name);

// The 225-character serialisation, for saving and for the .sav file so a game
// in progress keeps the board it was started on.
void gridToString(char *out);              // out must hold GRID_CELLS + 1
bool gridFromString(const char *s);
