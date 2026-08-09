// define.cpp — see define.h.

#include "define.h"
#include "configs.h"
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>

// Headwords in these files are in the game's language, so the direction picked
// is the one whose LEFT side is that language.
static const char *dictXml(uint8_t lang) {
  return (lang == 0) ? DICT_DIR "/de-en.xml" : DICT_DIR "/en-de.xml";
}
static const char *dictToc(uint8_t lang) {
  return (lang == 0) ? DICT_DIR "/de-en.toc" : DICT_DIR "/en-de.toc";
}

// Fold to a comparable key: upper-case letters only. The files sort
// case-insensitively and include spaces, hyphens and bracketed qualifiers in
// their headwords ("A-Filament", "Aal"), none of which a tile word can contain,
// so both sides are reduced to bare letters before comparing.
static void foldKey(const char *s, char *out, size_t n) {
  size_t o = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p && o + 1 < n; p++) {
    uint8_t c = *p;
    if (c == 0xC3 && p[1]) {                 // UTF-8 umlaut in the file
      uint8_t d = *++p;
      switch (d) {
        case 0x84: case 0xA4: out[o++] = 'A'; break;
        case 0x96: case 0xB6: out[o++] = 'O'; break;
        case 0x9C: case 0xBC: out[o++] = 'U'; break;
        case 0x9F: out[o++] = 'S'; if (o + 1 < n) out[o++] = 'S'; break;
        default: break;
      }
    } else if (c >= 'a' && c <= 'z') {
      out[o++] = c - 'a' + 'A';
    } else if (c >= 'A' && c <= 'Z') {
      out[o++] = c;
    }
  }
  out[o] = 0;
}

// The firmware's private letter bytes folded the same way (umlauts to their base
// letter), so a tile word and a file headword meet on common ground.
static void foldWord(const uint8_t *w, uint8_t len, char *out, size_t n) {
  size_t o = 0;
  for (uint8_t i = 0; i < len && o + 1 < n; i++) {
    switch (w[i]) {
      case 0x80: out[o++] = 'A'; break;
      case 0x82: out[o++] = 'O'; break;
      case 0x84: out[o++] = 'U'; break;
      default:   out[o++] = (char)w[i];
    }
  }
  out[o] = 0;
}

// Private bytes -> UTF-8, for the online query.
//
// Done here rather than by including vlw.h: that header carries the font tables,
// which are `static`, so pulling it into a second translation unit would put a
// duplicate ~100 KB of glyph data in flash.
static String toUtf8(const uint8_t *w, uint8_t len) {
  String s;
  for (uint8_t i = 0; i < len; i++) {
    // Written as numeric bytes rather than escapes so this file stays pure
    // ASCII and cannot be re-encoded by an editor.
    switch (w[i]) {
      case 0x80: s += (char)0xC3; s += (char)0x84; break;   // A-umlaut
      case 0x82: s += (char)0xC3; s += (char)0x96; break;   // O-umlaut
      case 0x84: s += (char)0xC3; s += (char)0x9C; break;   // U-umlaut
      default:   s += (char)w[i];
    }
  }
  return s;
}

// Byte range of a first-letter bucket, from the .toc.
static bool bucketRange(uint8_t lang, char letter, uint32_t &lo, uint32_t &hi) {
  File f = SD.open(dictToc(lang), FILE_READ);
  if (!f) return false;
  lo = hi = 0;
  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (!line.startsWith("B|")) continue;
    // B|<code>|<display>|<chapters>|<section>|<byteOffset>
    int p1 = line.indexOf('|', 2);
    if (p1 < 0) continue;
    String code = line.substring(2, p1);
    int last = line.lastIndexOf('|');
    if (last < 0) continue;
    uint32_t off = (uint32_t)strtoul(line.c_str() + last + 1, nullptr, 10);
    if (found) { hi = off; break; }          // next bucket bounds this one
    if (code.length() == 1 && code[0] == letter) { lo = off; found = true; }
  }
  f.close();
  return found;
}

// Read the headword of the first <verse> at or after `pos`; returns its offset.
static bool headwordAt(File &f, uint32_t pos, uint32_t limit, char *key, size_t keyN,
                       uint32_t &entryPos) {
  if (pos >= limit) return false;
  f.seek(pos);
  // Find the start of an entry.
  String tag = "";
  uint32_t p = pos;
  while (p < limit) {
    int c = f.read();
    if (c < 0) return false;
    p++;
    tag = tag.substring(tag.length() > 20 ? tag.length() - 20 : 0) + (char)c;
    if (tag.endsWith("\">")) break;          // end of the osisID attribute
  }
  if (p >= limit) return false;
  entryPos = p;
  // Read up to the " - " that separates headword from definition.
  char head[96];
  size_t n = 0;
  while (p < limit && n + 1 < sizeof(head)) {
    int c = f.read();
    if (c < 0) break;
    p++;
    if (c == '<') break;
    head[n++] = (char)c;
    if (n >= 3 && head[n - 3] == ' ' && head[n - 2] == '-' && head[n - 1] == ' ') {
      n -= 3;
      break;
    }
  }
  head[n] = 0;
  foldKey(head, key, keyN);
  return true;
}

bool defineOffline(uint8_t lang, const uint8_t *word, uint8_t len, String &out) {
  char target[32];
  foldWord(word, len, target, sizeof(target));
  if (!target[0]) return false;

  uint32_t lo, hi;
  if (!bucketRange(lang, target[0], lo, hi)) return false;

  File f = SD.open(dictXml(lang), FILE_READ);
  if (!f) return false;
  if (hi == 0 || hi > f.size()) hi = f.size();

  // Bisect the byte range until the window is small enough to scan.
  char key[96];
  uint32_t entry;
  while (hi - lo > 32768) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (!headwordAt(f, mid, hi, key, sizeof(key), entry)) break;
    if (strcmp(key, target) < 0) lo = mid; else hi = mid;
  }

  // Linear scan the window, gathering every sense of the word.
  f.seek(lo);
  String defs;
  uint8_t hits = 0;
  uint32_t p = lo;
  while (p < hi && hits < 6) {
    String line = f.readStringUntil('>');    // up to the end of <verse ...>
    p += line.length() + 1;
    if (line.indexOf("<verse") < 0) continue;
    String body = f.readStringUntil('<');
    p += body.length() + 1;
    int sep = body.indexOf(" - ");
    if (sep < 0) continue;
    char k[96];
    foldKey(body.substring(0, sep).c_str(), k, sizeof(k));
    int cmp = strcmp(k, target);
    if (cmp == 0) {
      if (defs.length()) defs += "\n";
      defs += body.substring(sep + 3);
      hits++;
    } else if (cmp > 0 && hits) {
      break;                                  // past it, and we already have some
    }
  }
  f.close();
  if (!hits) return false;
  out = defs;
  return true;
}

bool defineOnline(uint8_t lang, const uint8_t *word, uint8_t len, String &out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String w = toUtf8(word, len);
  w.toLowerCase();

  String url = String("https://api.dictionaryapi.dev/api/v2/entries/") +
               (lang == 0 ? "de" : "en") + "/" + w;

  NetworkClientSecure client;
  client.setInsecure();                       // no cert store on device
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();

  // [ { meanings: [ { partOfSpeech, definitions: [ { definition } ] } ] } ]
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) return false;
  String defs;
  uint8_t hits = 0;
  for (JsonVariant entry : doc.as<JsonArray>()) {
    for (JsonVariant m : entry["meanings"].as<JsonArray>()) {
      String pos = m["partOfSpeech"].as<String>();
      for (JsonVariant d : m["definitions"].as<JsonArray>()) {
        if (hits >= 6) break;
        if (defs.length()) defs += "\n";
        if (pos.length()) defs += "[" + pos + "] ";
        defs += d["definition"].as<String>();
        hits++;
      }
    }
  }
  if (!hits) return false;
  out = defs;
  return true;
}

bool defineWord(uint8_t lang, const uint8_t *word, uint8_t len,
                bool allowOnline, String &out, bool &fromOnline) {
  fromOnline = false;
  if (defineOffline(lang, word, len, out)) return true;
  if (allowOnline && defineOnline(lang, word, len, out)) {
    fromOnline = true;
    return true;
  }
  return false;
}
