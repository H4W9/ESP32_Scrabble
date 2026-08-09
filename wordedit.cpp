// wordedit.cpp — see wordedit.h.

#include "wordedit.h"
#include "configs.h"
#include <SD.h>

namespace {

struct WordList {
  uint8_t w[WE_MAX_WORDS][WE_MAX_LEN];
  uint8_t len[WE_MAX_WORDS];
  uint8_t n = 0;
};

// [lang][0] = blocked, [lang][1] = allowed.
WordList g_list[2][2];
bool     g_dirty[2] = { false, false };

int slot(WordStatus st) { return (st == WS_BLOCKED) ? 0 : 1; }

int find(const WordList &L, const uint8_t *w, uint8_t len) {
  for (uint8_t i = 0; i < L.n; i++)
    if (L.len[i] == len && !memcmp(L.w[i], w, len)) return i;
  return -1;
}

bool add(WordList &L, const uint8_t *w, uint8_t len) {
  if (find(L, w, len) >= 0) return true;
  if (L.n >= WE_MAX_WORDS || len > WE_MAX_LEN) return false;
  memcpy(L.w[L.n], w, len);
  L.len[L.n] = len;
  L.n++;
  return true;
}

void remove(WordList &L, int i) {
  if (i < 0 || i >= L.n) return;
  for (uint8_t k = i; k + 1 < L.n; k++) {
    memcpy(L.w[k], L.w[k + 1], WE_MAX_LEN);
    L.len[k] = L.len[k + 1];
  }
  L.n--;
}

const char *path(uint8_t lang) {
  return (lang == 0) ? SCRABBLE_DIR "/words_de.txt" : SCRABBLE_DIR "/words_en.txt";
}

// One word per line, prefixed '-' blocked or '+' allowed. Letters are the
// firmware's own bytes, so the file is not meant to be edited by hand — the
// umlauts would not survive a text editor.
void loadLang(uint8_t lang) {
  g_list[lang][0].n = g_list[lang][1].n = 0;
  File f = SD.open(path(lang), FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;
    char tag = line[0];
    if (tag != '-' && tag != '+') continue;
    uint8_t buf[WE_MAX_LEN];
    uint8_t n = 0;
    for (int i = 1; i < (int)line.length() && n < WE_MAX_LEN; i++) buf[n++] = (uint8_t)line[i];
    add(g_list[lang][tag == '-' ? 0 : 1], buf, n);
  }
  f.close();
}

} // namespace

void weBegin() {
  loadLang(0);
  loadLang(1);
}

WordStatus weStatus(uint8_t lang, const uint8_t *w, uint8_t len) {
  lang &= 1;
  if (find(g_list[lang][0], w, len) >= 0) return WS_BLOCKED;
  if (find(g_list[lang][1], w, len) >= 0) return WS_ALLOWED;
  return WS_NORMAL;
}

bool wordAllowed(uint8_t lang, const uint8_t *w, uint8_t len, bool inDict) {
  switch (weStatus(lang, w, len)) {
    case WS_BLOCKED: return false;
    case WS_ALLOWED: return true;
    default:         return inDict;
  }
}

bool weSet(uint8_t lang, const uint8_t *w, uint8_t len, WordStatus st) {
  lang &= 1;
  if (!len || len > WE_MAX_LEN) return false;
  // A word is in at most one list, so clear it from both first.
  for (uint8_t s = 0; s < 2; s++) remove(g_list[lang][s], find(g_list[lang][s], w, len));
  bool ok = (st == WS_NORMAL) ? true : add(g_list[lang][slot(st)], w, len);
  g_dirty[lang] = true;
  weSave(lang);
  return ok;
}

uint8_t weCount(uint8_t lang, WordStatus which) {
  return g_list[lang & 1][slot(which)].n;
}

uint8_t weAt(uint8_t lang, WordStatus which, uint8_t i, uint8_t *out) {
  const WordList &L = g_list[lang & 1][slot(which)];
  if (i >= L.n) return 0;
  memcpy(out, L.w[i], L.len[i]);
  return L.len[i];
}

void weClear(uint8_t lang, WordStatus which) {
  g_list[lang & 1][slot(which)].n = 0;
  g_dirty[lang & 1] = true;
  weSave(lang);
}

bool weSave(uint8_t lang) {
  lang &= 1;
  SD.mkdir(SCRABBLE_DIR);
  File f = SD.open(path(lang), FILE_WRITE);
  if (!f) return false;
  for (uint8_t s = 0; s < 2; s++) {
    const WordList &L = g_list[lang][s];
    for (uint8_t i = 0; i < L.n; i++) {
      f.write((uint8_t)(s == 0 ? '-' : '+'));
      f.write(L.w[i], L.len[i]);
      f.write('\n');
    }
  }
  f.close();
  g_dirty[lang] = false;
  return true;
}
