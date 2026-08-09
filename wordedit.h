// wordedit.h
// Per-language overrides on top of the loaded word list.
//
// The dictionary editor stores the EDIT, not a rebuilt dictionary: you look a
// word up and block or allow it, and a small pair of lists records those
// changes. So there are two small lists per language:
//
//   BLOCKED   words the DAWG accepts that you don't want played
//   ALLOWED   words the DAWG rejects that you do
//
// A LIMIT WORTH KNOWING: blocking works everywhere, because every acceptance
// goes through wordAllowed(). Allowing only works for words YOU play. The
// opponent and Master find their moves by walking the DAWG itself, so a word
// that isn't in the DAWG is a word they cannot reach — an allowed word is
// playable by you but will never be suggested or played against you. Rebuilding
// the .dwg is the only way to change that, and that is a job for sd_prep.

#pragma once
#include <Arduino.h>

#define WE_MAX_WORDS 128    // per list, per language
#define WE_MAX_LEN   15

enum WordStatus : uint8_t { WS_NORMAL = 0, WS_BLOCKED, WS_ALLOWED };

// Load both languages from SD. Safe to call before the card is ready — it just
// comes up empty.
void weBegin();

// What the editor has said about this word.
WordStatus weStatus(uint8_t lang, const uint8_t *w, uint8_t len);

// The verdict the game should use: `inDict` is what the DAWG said.
bool wordAllowed(uint8_t lang, const uint8_t *w, uint8_t len, bool inDict);

// Move a word between the three states. Returns false only if the list is full.
bool weSet(uint8_t lang, const uint8_t *w, uint8_t len, WordStatus st);

// Browsing, for the editor screen. `which` is WS_BLOCKED or WS_ALLOWED.
uint8_t weCount(uint8_t lang, WordStatus which);
uint8_t weAt(uint8_t lang, WordStatus which, uint8_t i, uint8_t *out);
void    weClear(uint8_t lang, WordStatus which);

bool weSave(uint8_t lang);
