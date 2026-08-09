// letters.cpp — see letters.h.

#include "letters.h"
#include "game.h"          // TILE_BLANK
#include <SPIFFS.h>

LetterDist g_dist;

// Built-in defaults.
//
// English uses its own scale, roughly 3x Scrabble's: A/E/I/O = 3 rather than 1.
// Counts sum to 100 letters + 2 blanks.
//
// German is the official German Scrabble (SDeV) 102-tile set, on a different
// scale from the English set, so the two languages are NOT comparable on score.
static const TileDef DEF_EN[] = {
  {'A',3,9},{'B',6,2},{'C',6,2},{'D',4,4},{'E',3,13},{'F',6,2},{'G',5,2},
  {'H',6,3},{'I',3,8},{'J',12,1},{'K',7,1},{'L',3,4},{'M',6,2},{'N',3,6},
  {'O',3,8},{'P',6,2},{'Q',12,1},{'R',3,6},{'S',3,5},{'T',3,7},{'U',4,4},
  {'V',7,2},{'W',6,2},{'X',10,1},{'Y',6,2},{'Z',12,1},
  {TILE_BLANK,0,2},
};

static const TileDef DEF_DE[] = {
  {'A',1,5},{'B',3,2},{'C',4,2},{'D',1,4},{'E',1,15},{'F',4,2},{'G',2,3},
  {'H',2,4},{'I',1,6},{'J',6,1},{'K',4,2},{'L',2,3},{'M',3,4},{'N',1,9},
  {'O',2,3},{'P',4,1},{'Q',10,1},{'R',1,6},{'S',1,7},{'T',1,6},{'U',1,6},
  {'V',6,1},{'W',3,1},{'X',8,1},{'Y',10,1},{'Z',3,1},
  {0x80,6,1},{0x82,8,1},{0x84,6,1},          // Ae, Oe, Ue
  {TILE_BLANK,0,2},
};

static const TileDef *defaultsFor(uint8_t lang, uint8_t &n) {
  if ((lang & 1) == 0) { n = sizeof(DEF_DE) / sizeof(DEF_DE[0]); return DEF_DE; }
  n = sizeof(DEF_EN) / sizeof(DEF_EN[0]);   return DEF_EN;
}

static const char *pathFor(uint8_t lang) {
  return (lang & 1) == 0 ? "/scrab_dist_de.dat" : "/scrab_dist_en.dat";
}

void LetterDist::loadDefaults(uint8_t lang) {
  uint8_t l = lang & 1, n;
  const TileDef *d = defaultsFor(l, n);
  if (n > DIST_MAX_ENTRIES) n = DIST_MAX_ENTRIES;
  for (uint8_t i = 0; i < n; i++) _d[l][i] = d[i];
  _n[l] = n;
  _bingo[l] = DIST_DEFAULT_BINGO;
}

void LetterDist::begin() {
  for (uint8_t l = 0; l < 2; l++)
    if (!load(l)) loadDefaults(l);
}

void LetterDist::reset(uint8_t lang) { loadDefaults(lang); }

uint8_t LetterDist::valueOf(uint8_t lang, uint8_t letter) const {
  uint8_t l = lang & 1;
  for (uint8_t i = 0; i < _n[l]; i++)
    if (_d[l][i].letter == letter) return _d[l][i].value;
  return 0;
}

uint16_t LetterDist::totalTiles(uint8_t lang) const {
  uint8_t l = lang & 1;
  uint16_t t = 0;
  for (uint8_t i = 0; i < _n[l]; i++) t += _d[l][i].count;
  return t;
}

bool LetterDist::edited(uint8_t lang) const {
  uint8_t l = lang & 1, n;
  const TileDef *d = defaultsFor(l, n);
  if (_bingo[l] != DIST_DEFAULT_BINGO) return true;
  if (n != _n[l]) return true;
  for (uint8_t i = 0; i < n; i++)
    if (_d[l][i].letter != d[i].letter || _d[l][i].value != d[i].value ||
        _d[l][i].count  != d[i].count) return true;
  return false;
}

// Persistence: a tiny versioned blob. The letter byte is stored alongside the
// numbers so a future change to the alphabet can't silently misalign an old file.
#define DIST_MAGIC 0xD15D

bool LetterDist::save(uint8_t lang) const {
  uint8_t l = lang & 1;
  File f = SPIFFS.open(pathFor(l), FILE_WRITE);
  if (!f) return false;
  uint16_t magic = DIST_MAGIC;
  f.write((const uint8_t *)&magic, 2);
  f.write(_n[l]);
  for (uint8_t i = 0; i < _n[l]; i++) {
    f.write(_d[l][i].letter);
    f.write(_d[l][i].value);
    f.write(_d[l][i].count);
  }
  f.write((const uint8_t *)&_bingo[l], 2);   // appended: older files just end here
  f.close();
  return true;
}

bool LetterDist::load(uint8_t lang) {
  uint8_t l = lang & 1;
  File f = SPIFFS.open(pathFor(l), FILE_READ);
  if (!f) return false;
  uint16_t magic = 0;
  if (f.read((uint8_t *)&magic, 2) != 2 || magic != DIST_MAGIC) { f.close(); return false; }
  int n = f.read();
  if (n <= 0 || n > DIST_MAX_ENTRIES) { f.close(); return false; }
  for (int i = 0; i < n; i++) {
    int a = f.read(), b = f.read(), c = f.read();
    if (a < 0 || b < 0 || c < 0) { f.close(); return false; }
    _d[l][i] = { (uint8_t)a, (uint8_t)b, (uint8_t)c };
  }
  _bingo[l] = DIST_DEFAULT_BINGO;
  if (f.available() >= 2) {                  // present only in newer files
    uint16_t b = 0;
    f.read((uint8_t *)&b, 2);
    if (b <= DIST_MAX_BINGO) _bingo[l] = b;
  }
  f.close();
  _n[l] = (uint8_t)n;
  return true;
}
