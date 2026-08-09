// boardgrid.cpp — see boardgrid.h.

#include "boardgrid.h"
#include "board_theme.h"
#include "configs.h"
#include <SD.h>

uint8_t g_grid[GRID_CELLS];
char    g_gridName[GRID_NAME_MAX] = "Classic";

#define GRID_DIR SCRABBLE_DIR "/grids"

// The default board — the layout board_theme.h used to hard-code.
// Kept as the character form because that is also the on-disk format.
static const char CLASSIC[GRID_CELLS + 1] =
    "L...W..l..W...L"
    ".l...L...L...l."
    "..w...l.l...w.."
    "...L...w...L..."
    "W...w.l.l.w...W"
    ".L...L...L...L."
    "..l.l.....l.l.."
    "l..w...*...w..l"
    "..l.l.....l.l.."
    ".L...L...L...L."
    "W...w.l.l.w...W"
    "...L...w...L..."
    "..w...l.l...w.."
    ".l...L...L...l."
    "L...W..l..W...L";

static uint8_t codeToPremium(char c) {
  switch (c) {
    case 'l': return PR_DL;
    case 'L': return PR_TL;
    case 'w': return PR_DW;
    case 'W': return PR_TW;
    case '*': return PR_CENTRE;
    default:  return PR_NONE;
  }
}
static char premiumToCode(uint8_t p) {
  switch (p) {
    case PR_DL:     return 'l';
    case PR_TL:     return 'L';
    case PR_DW:     return 'w';
    case PR_TW:     return 'W';
    case PR_CENTRE: return '*';
    default:        return '.';
  }
}

void gridToString(char *out) {
  for (uint16_t i = 0; i < GRID_CELLS; i++) out[i] = premiumToCode(g_grid[i]);
  out[GRID_CELLS] = 0;
}

bool gridFromString(const char *s) {
  if (!s || strlen(s) < GRID_CELLS) return false;
  for (uint16_t i = 0; i < GRID_CELLS; i++) g_grid[i] = codeToPremium(s[i]);
  g_grid[(GRID_N / 2) * GRID_N + GRID_N / 2] = PR_CENTRE;   // the star is not optional
  return true;
}

// Built-ins
//
// Classic is the default fixed layout. "Random" is not a second design, it is a
// fresh board each time it is picked, built to Classic's premium budget.
static const char *const BUILTIN_NAMES[] = { "Classic", "Random" };

uint8_t     gridBuiltinCount()          { return 2; }
const char *gridBuiltinName(uint8_t i)  { return (i < 2) ? BUILTIN_NAMES[i] : ""; }

bool gridLoadBuiltin(uint8_t i) {
  if (i == 0) {
    gridFromString(CLASSIC);
    strncpy(g_gridName, "Classic", GRID_NAME_MAX - 1);
    g_gridName[GRID_NAME_MAX - 1] = 0;
    return true;
  }
  if (i == 1) { gridGenerateRandom(); return true; }
  return false;
}

void gridCounts(uint8_t out[6]) {
  for (uint8_t i = 0; i < 6; i++) out[i] = 0;
  for (uint16_t i = 0; i < GRID_CELLS; i++)
    if (g_grid[i] < 6) out[g_grid[i]]++;
}

uint8_t gridNextBonus(uint8_t p) {
  switch (p) {
    case PR_NONE: return PR_DL;
    case PR_DL:   return PR_TL;
    case PR_TL:   return PR_DW;
    case PR_DW:   return PR_TW;
    default:      return PR_NONE;
  }
}

// The eight squares a cell maps to under the board's symmetry group: both
// mirrors and the transpose. Duplicates are fine — they just get written twice.
static void orbit(uint8_t r, uint8_t c, uint8_t rr[8], uint8_t cc[8]) {
  const uint8_t m = GRID_N - 1;
  uint8_t sr[8] = { r, r, (uint8_t)(m - r), (uint8_t)(m - r), c, c, (uint8_t)(m - c), (uint8_t)(m - c) };
  uint8_t sc[8] = { c, (uint8_t)(m - c), c, (uint8_t)(m - c), r, (uint8_t)(m - r), r, (uint8_t)(m - r) };
  for (uint8_t i = 0; i < 8; i++) { rr[i] = sr[i]; cc[i] = sc[i]; }
}

void gridPaint(uint8_t r, uint8_t c, uint8_t p) {
  if (r >= GRID_N || c >= GRID_N) return;
  const uint8_t mid = GRID_N / 2;
  if (r == mid && c == mid) return;                 // the centre star is fixed
  uint8_t rr[8], cc[8];
  orbit(r, c, rr, cc);
  for (uint8_t i = 0; i < 8; i++) {
    if (rr[i] == mid && cc[i] == mid) continue;
    g_grid[rr[i] * GRID_N + cc[i]] = p;
  }
}

// A fresh board to Classic's budget.
//
// Placement is by symmetry orbit rather than by cell, so the result is symmetric
// by construction. Orbits are shuffled and then filled in descending order of
// premium strength; a triple word is kept off the squares adjacent to the centre
// star, which is the one placement rule that visibly separates a sensible board
// from a random one (a TW next to the star is reachable on move one).
void gridGenerateRandom() {
  for (uint16_t i = 0; i < GRID_CELLS; i++) g_grid[i] = PR_NONE;
  const uint8_t mid = GRID_N / 2;
  g_grid[mid * GRID_N + mid] = PR_CENTRE;

  // One representative per orbit: the upper-left triangle of the top-left
  // quadrant covers every orbit exactly once.
  struct Cell { uint8_t r, c, n; };
  static Cell rep[64];
  uint8_t nrep = 0;
  for (uint8_t r = 0; r <= mid && nrep < 64; r++)
    for (uint8_t c = r; c <= mid && nrep < 64; c++) {
      if (r == mid && c == mid) continue;
      // How many distinct squares this orbit actually covers — cells on a
      // mirror line or the diagonal map onto themselves, so it is not always 8.
      uint8_t rr[8], cc[8]; orbit(r, c, rr, cc);
      uint8_t n = 0;
      for (uint8_t i = 0; i < 8; i++) {
        bool dup = false;
        for (uint8_t j = 0; j < i && !dup; j++) dup = (rr[j] == rr[i] && cc[j] == cc[i]);
        if (!dup) n++;
      }
      rep[nrep++] = { r, c, n };
    }

  for (int i = nrep - 1; i > 0; i--) {              // shuffle the orbits
    int j = random(i + 1);
    Cell t = rep[i]; rep[i] = rep[j]; rep[j] = t;
  }

  // Classic's budget, counted from the layout above.
  uint8_t want[6] = { 0, 0, 0, 0, 0, 0 };
  {
    uint8_t save[GRID_CELLS];
    memcpy(save, g_grid, GRID_CELLS);
    gridFromString(CLASSIC);
    gridCounts(want);
    memcpy(g_grid, save, GRID_CELLS);
  }

  const uint8_t order[4] = { PR_TW, PR_DW, PR_TL, PR_DL };
  uint8_t next = 0;
  for (uint8_t oi = 0; oi < 4; oi++) {
    uint8_t kind = order[oi];
    int left = want[kind];
    while (left > 0 && next < nrep) {
      Cell &cell = rep[next++];
      // Keep a triple word out of the star's reach on the opening move.
      if (kind == PR_TW) {
        int dr = (int)cell.r - mid, dc = (int)cell.c - mid;
        if (dr * dr + dc * dc < 16) continue;
      }
      if ((int)cell.n > left + 2) continue;         // orbit too big for the budget
      gridPaint(cell.r, cell.c, kind);
      left -= cell.n;
    }
  }
  strncpy(g_gridName, "Random", GRID_NAME_MAX - 1);
  g_gridName[GRID_NAME_MAX - 1] = 0;
}

// Saved layouts
static void gridPath(const char *name, char *out, size_t n) {
  snprintf(out, n, GRID_DIR "/%s.grd", name);
}

uint8_t gridListSaved(String *out, uint8_t maxN) {
  File dir = SD.open(GRID_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  uint8_t n = 0;
  for (File f = dir.openNextFile(); f && n < maxN; f = dir.openNextFile()) {
    String nm = f.name();
    int slash = nm.lastIndexOf('/');
    if (slash >= 0) nm = nm.substring(slash + 1);
    if (nm.endsWith(".grd")) out[n++] = nm.substring(0, nm.length() - 4);
    f.close();
  }
  dir.close();
  return n;
}

bool gridSave(const char *name) {
  if (!name || !name[0]) return false;
  SD.mkdir(SCRABBLE_DIR);
  SD.mkdir(GRID_DIR);
  char path[64]; gridPath(name, path, sizeof(path));
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  char s[GRID_CELLS + 1];
  gridToString(s);
  f.write((const uint8_t *)s, GRID_CELLS);
  f.close();
  strncpy(g_gridName, name, GRID_NAME_MAX - 1);
  g_gridName[GRID_NAME_MAX - 1] = 0;
  return true;
}

bool gridLoad(const char *name) {
  if (!name || !name[0]) return false;
  char path[64]; gridPath(name, path, sizeof(path));
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  char s[GRID_CELLS + 1];
  size_t got = f.read((uint8_t *)s, GRID_CELLS);
  f.close();
  if (got < GRID_CELLS) return false;
  s[GRID_CELLS] = 0;
  if (!gridFromString(s)) return false;
  strncpy(g_gridName, name, GRID_NAME_MAX - 1);
  g_gridName[GRID_NAME_MAX - 1] = 0;
  return true;
}

bool gridDelete(const char *name) {
  char path[64]; gridPath(name, path, sizeof(path));
  return SD.remove(path);
}

void gridBegin(const char *name) {
  gridLoadBuiltin(0);                               // Classic, so g_grid is never junk
  if (!name || !name[0]) return;
  if (!strcmp(name, "Classic")) return;
  if (!strcmp(name, "Random")) { gridGenerateRandom(); return; }
  gridLoad(name);                                   // leaves Classic loaded if it fails
}
