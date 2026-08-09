// define.h
// Word definitions, offline from the SD dictionaries and online over WiFi.
//
// OFFLINE uses the very same `/dictionary/*.xml` files the ESP32 Library reader
// ships — 120-160 MB of "headword - definition" entries — plus their `.toc`,
// which gives the byte offset of each first-letter bucket. Entries are sorted
// inside a bucket, so a lookup is a BINARY SEARCH ON BYTE OFFSET: seek to the
// middle, scan forward to the next <verse>, compare headwords, halve. About 20
// seeks instead of seeking through several MB, which is what makes this usable
// on an SD card.
//
// (This is the same data the .pgx page index was built for, but the page index
// only narrows to a 100-entry page and still needs a forward scan from the
// bucket start. Bisecting the byte range directly is both simpler and faster.)
//
// ONLINE is a fallback for words the local dictionary lacks.

#pragma once
#include <Arduino.h>

// Look a word up. `word` is in the firmware's letter bytes (A-Z, 0x80 Ae,
// 0x82 Oe, 0x84 Ue); `lang` is LANG_DE / LANG_EN.
//
// Returns true and fills `out` with the definition text. `fromOnline` reports
// which source answered, so the UI can say so.
bool defineWord(uint8_t lang, const uint8_t *word, uint8_t len,
                bool allowOnline, String &out, bool &fromOnline);

// Definition sources, exposed for the UI's status line.
bool defineOffline(uint8_t lang, const uint8_t *word, uint8_t len, String &out);
bool defineOnline(uint8_t lang, const uint8_t *word, uint8_t len, String &out);
