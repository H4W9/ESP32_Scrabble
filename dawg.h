// dawg.h
// Loader + walker for the word lists built by sd_prep/generate_scrabble_dict.py.
//
// The whole edge array is pulled into PSRAM once at load and never touched on SD
// again, because move generation (M3) walks it thousands of times per CPU turn
// and SD seeks would make that hopeless. de.dwg is ~2.1 MB and en.dwg ~2.4 MB,
// which the Pancake's PSRAM swallows comfortably.
//
// Format is documented in PLAN.md and in the generator's docstring. Briefly:
//   header 20 bytes, then N alphabet bytes, then nedges * uint32 edges
//   edge: bit31 end-of-word, bit30 last-edge-of-node,
//         bits29-24 letter index, bits23-0 target node (0 = leaf)
// A node is a run of consecutive edges terminated by the bit30 edge, so a node
// is addressed by the index of its first edge.

#pragma once
#include <Arduino.h>
#include <SD.h>

#define DAWG_MAX_ALPHA 32

class Dawg {
public:
  ~Dawg() { unload(); }

  bool loaded() const { return _edges != nullptr; }
  uint32_t wordCount() const { return _nwords; }
  uint32_t edgeCount() const { return _nedges; }
  uint8_t  alphaSize() const { return _nalpha; }
  const char *error() const { return _err; }

  // Bytes actually occupied by the edge array (what shows up in the PSRAM readout).
  size_t bytes() const { return (size_t)_nedges * 4; }

  void unload() {
    if (_edges) { free(_edges); _edges = nullptr; }
    _nedges = _nwords = 0; _root = 0; _nalpha = 0;
  }

  bool load(const char *path) {
    unload();
    _err = "";
    File f = SD.open(path, FILE_READ);
    if (!f) { _err = "open failed"; return false; }

    uint8_t hdr[20];
    if (f.read(hdr, 20) != 20) { _err = "short header"; f.close(); return false; }
    if (memcmp(hdr, "DWG1", 4) != 0) { _err = "bad magic"; f.close(); return false; }

    _nalpha = hdr[4];
    if (_nalpha == 0 || _nalpha > DAWG_MAX_ALPHA) { _err = "bad alphabet"; f.close(); return false; }
    _root   = rd32(hdr + 8);
    _nedges = rd32(hdr + 12);
    _nwords = rd32(hdr + 16);
    if (_nedges == 0) { _err = "empty"; f.close(); return false; }

    if (f.read(_alpha, _nalpha) != _nalpha) { _err = "short alphabet"; f.close(); return false; }
    // Reverse map letter byte -> index, so contains() is a table lookup per char
    // instead of a scan over the alphabet.
    memset(_idx, 0xFF, sizeof(_idx));
    for (uint8_t i = 0; i < _nalpha; i++) _idx[_alpha[i]] = i;

    size_t need = (size_t)_nedges * 4;
    _edges = (uint32_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!_edges) _edges = (uint32_t *)malloc(need);   // no PSRAM: try DRAM, likely fails
    if (!_edges) { _err = "out of memory"; f.close(); return false; }

    // Read in chunks; a single 2 MB read is slower and can fail on some cards.
    uint8_t *dst = (uint8_t *)_edges;
    size_t got = 0;
    while (got < need) {
      size_t want = need - got;
      if (want > 32768) want = 32768;
      int r = f.read(dst + got, want);
      if (r <= 0) { _err = "short read"; f.close(); unload(); return false; }
      got += r;
    }
    f.close();
    return true;
  }

  // Is `w` (length `len`, firmware letter bytes) a word?
  bool contains(const uint8_t *w, uint8_t len) const {
    if (!_edges || len == 0) return false;
    uint32_t node = _root;
    for (uint8_t k = 0; k < len; k++) {
      uint8_t li = _idx[w[k]];
      if (li == 0xFF) return false;                 // letter not in this alphabet
      uint32_t e;
      if (!findEdge(node, li, e)) return false;
      if (k == len - 1) return endOfWord(e);
      uint32_t t = target(e);
      if (!t) return false;
      node = t;
    }
    return false;
  }

  bool contains(const char *s) const {
    return contains((const uint8_t *)s, (uint8_t)strlen(s));
  }

  // --- raw access, for the M3 move generator ---
  uint32_t root() const { return _root; }
  uint32_t edge(uint32_t i) const { return _edges[i]; }
  static bool     endOfWord(uint32_t e) { return (e >> 31) & 1; }
  static bool     lastEdge(uint32_t e)  { return (e >> 30) & 1; }
  static uint8_t  letterIdx(uint32_t e) { return (e >> 24) & 0x3F; }
  static uint32_t target(uint32_t e)    { return e & 0xFFFFFF; }
  uint8_t letterByte(uint8_t idx) const { return idx < _nalpha ? _alpha[idx] : 0; }
  // Alphabet index of a letter byte, or -1 if it isn't in this language's set.
  int8_t  indexOf(uint8_t letter) const {
    uint8_t i = _idx[letter];
    return (i == 0xFF) ? -1 : (int8_t)i;
  }

  // Linear scan of a node's edge run for `li`. Edge runs are short (<= alphabet
  // size, usually a handful) so this beats a binary search in practice.
  bool findEdge(uint32_t node, uint8_t li, uint32_t &out) const {
    for (uint32_t i = node; i < _nedges; i++) {
      uint32_t e = _edges[i];
      if (letterIdx(e) == li) { out = e; return true; }
      if (lastEdge(e)) return false;
    }
    return false;
  }

private:
  static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }

  uint32_t *_edges = nullptr;
  uint32_t  _nedges = 0, _nwords = 0, _root = 0;
  uint8_t   _nalpha = 0;
  uint8_t   _alpha[DAWG_MAX_ALPHA];
  uint8_t   _idx[256];
  const char *_err = "";
};
