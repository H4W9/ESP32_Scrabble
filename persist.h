// persist.h
// Settings + statistics storage backend.
//
// Historically the UI theme, game config and stats all lived on SPIFFS. They now
// prefer the SD card (mirroring the game-save policy in the .ino: "SD if present,
// else SPIFFS") so a player's themes and records travel with the card and survive
// a firmware re-flash. SPIFFS remains the fallback when no card is mounted.
//
// On SD the files live under PERSIST_DIR (e.g. /scrabble/pico_ui.dat). On SPIFFS
// they keep their historical /name path, so an existing install still reads its
// old settings — and because persistRead() falls back to SPIFFS when the SD copy
// is missing, the first save after inserting a card silently migrates them across.
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include "configs.h"
#ifdef USE_SD
#include <SD.h>
#endif

// Directory on SD that holds settings + stats (created on demand). Each app's
// configs.h already defines its data dir; reuse it so everything sits together.
#ifndef PERSIST_DIR
#  if defined(SCRABBLE_DIR)
#    define PERSIST_DIR SCRABBLE_DIR
#  elif defined(SUDOKU_DIR)
#    define PERSIST_DIR SUDOKU_DIR
#  else
#    define PERSIST_DIR "/data"
#  endif
#endif

// Set true by setup() once an SD card has mounted. Defined in the .ino.
extern bool g_persistSD;

// Open a settings/stats file for reading by bare name ("pico_ui.dat"). Prefers
// the SD copy under PERSIST_DIR; if the card is absent — or present but the file
// has not been written to it yet — falls back to the SPIFFS /name path.
static inline File persistRead(const char *name) {
#ifdef USE_SD
  if (g_persistSD) {
    File f = SD.open((String(PERSIST_DIR) + "/" + name).c_str(), FILE_READ);
    if (f) return f;                     // else fall through (first run / migration)
  }
#endif
  return SPIFFS.open((String("/") + name).c_str(), FILE_READ);
}

// Open a settings/stats file for writing (truncating). Writes to SD under
// PERSIST_DIR when a card is present, else to the SPIFFS /name path.
static inline File persistWrite(const char *name) {
#ifdef USE_SD
  if (g_persistSD) {
    SD.mkdir(PERSIST_DIR);
    return SD.open((String(PERSIST_DIR) + "/" + name).c_str(), FILE_WRITE);
  }
#endif
  return SPIFFS.open((String("/") + name).c_str(), FILE_WRITE);
}
