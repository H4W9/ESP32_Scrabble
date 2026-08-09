// cpu.cpp — see cpu.h for the algorithm.

#include "cpu.h"
#include "letters.h"
#include "wordedit.h"

#define KEEP_BEST 16          // top-N moves retained, so difficulty can pick down the list

static CpuAbortFn g_abortHook = nullptr;
static bool       g_aborted = false;
void cpuSetAbortHook(CpuAbortFn fn) { g_abortHook = fn; }
bool cpuWasAborted() { return g_aborted; }

namespace {

// Polled from the recursion. Rate-limited: the hook reads the touch panel over
// I2C, which is far too slow to do on every node.
bool abortRequested() {
  if (!g_abortHook) return false;
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 80) return false;
  last = now;
  if (!g_abortHook()) return false;
  g_aborted = true;
  return true;
}


// Search state, bundled so the recursion doesn't need a dozen parameters.
struct Search {
  Game       *g;
  const Dawg *d;
  bool        horiz;
  uint8_t     line;                    // row when horizontal, column when vertical
  uint32_t    cross[BOARD_N];          // per-offset allowed-letter mask for this line
  bool        anchor[BOARD_N];
  bool        used[RACK_N];            // rack slots consumed by the current partial word

  // Top-N is kept SEPARATELY PER ORIENTATION, indexed [horiz?0:1]. A single
  // shared list let whichever orientation ran first fill all 16 slots and raise
  // the bar the other had to clear — the real reason the opponent kept playing
  // horizontally even after the search order and time budget were made fair.
  CpuMove     best[2][KEEP_BEST];
  uint8_t     nbest[2] = { 0, 0 };
  uint16_t    found[2] = { 0, 0 };      // diagnostics: candidates seen per axis
  bool        starved[2] = { false, false };

  uint32_t    deadline = 0;
  bool        expired = false;
  uint8_t     rejected = 0;             // non-words caught by the final vet
};

// Direction-independent square mapping. Everything else in the file works in
// (line, offset) space so the same code serves both orientations.
inline void mapSq(bool horiz, uint8_t line, int off, uint8_t &r, uint8_t &c) {
  if (horiz) { r = line; c = (uint8_t)off; }
  else       { r = (uint8_t)off; c = line; }
}

bool occupied(Game &g, bool horiz, uint8_t line, int off) {
  if (off < 0 || off >= BOARD_N) return false;
  uint8_t r, c; mapSq(horiz, line, off, r, c);
  return !g.isEmpty(r, c);
}

uint8_t letterAt(Game &g, bool horiz, uint8_t line, int off) {
  uint8_t r, c; mapSq(horiz, line, off, r, c);
  return g.at(r, c);
}

// Is this square's neighbourhood empty? A tile laid here opens new territory
// rather than thickening an existing cluster.
bool opensGround(Game &g, uint8_t r, uint8_t c) {
  const int8_t dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
  for (uint8_t k = 0; k < 4; k++) {
    int ar = (int)r + dr[k], ac = (int)c + dc[k];
    if (ar < 0 || ac < 0 || ar >= BOARD_N || ac >= BOARD_N) continue;
    if (!g.isEmpty((uint8_t)ar, (uint8_t)ac) && !g.isPendingAt((uint8_t)ar, (uint8_t)ac))
      return false;
  }
  return true;
}

// The main word of the candidate currently on the board, read along the play
// axis. Returns its length and fills `out`.
//
// Uses shownAt(), NOT at()/isEmpty(): those two read _board alone, and the
// candidate is still PENDING while the search is looking at it. Walking with
// at() here found only the committed tiles, so a play on fresh squares measured
// as length zero.
uint8_t mainWord(Search &s, uint8_t startR, uint8_t startC, uint8_t *out) {
  Game &g = *s.g;
  int8_t dr = s.horiz ? 0 : 1, dc = s.horiz ? 1 : 0;
  int sr = startR, sc = startC;
  while (true) {
    int pr = sr - dr, pc = sc - dc;
    if (pr < 0 || pc < 0 || pr >= BOARD_N || pc >= BOARD_N) break;
    if (g.shownAt((uint8_t)pr, (uint8_t)pc) == TILE_EMPTY) break;
    sr = pr; sc = pc;
  }
  uint8_t n = 0;
  while (sr >= 0 && sc >= 0 && sr < BOARD_N && sc < BOARD_N && n < BOARD_N) {
    uint8_t l = g.shownAt((uint8_t)sr, (uint8_t)sc);
    if (l == TILE_EMPTY) break;
    out[n++] = l;
    sr += dr; sc += dc;
  }
  return n;
}

// Fill in the evaluation inputs while the candidate is still on the board.
void gatherInfo(Search &s, CpuMove &m) {
  Game &g = *s.g;

  // The leave: rack slots this move does not consume.
  m.nleave = 0;
  for (uint8_t k = 0; k < RACK_N; k++) {
    if (s.used[k]) continue;
    uint8_t t = g.player(g.current()).rack[k];
    if (t != TILE_EMPTY) m.leave[m.nleave++] = t;
  }

  m.onEmpty = 0;
  for (uint8_t i = 0; i < m.n; i++)
    if (opensGround(g, m.p[i].row, m.p[i].col)) m.onEmpty++;

  uint8_t w[BOARD_N];
  m.wordLen = mainWord(s, m.p[0].row, m.p[0].col, w);

  // An S laid at the end of a word that already existed is almost always just a
  // pluralisation — a cheap play that spends the rack's most useful tile.
  m.sOnEnd = (m.n == 1 && m.wordLen > 2 &&
              m.p[0].letter == 'S' && w[m.wordLen - 1] == 'S');
}

// How good a move is, beyond its raw score.
//
// On top of the score, this weighs: tiles laid on empty ground, high-value
// letters held back, vowel/consonant balance, repeated letters, a Q without a U,
// a held blank, and the endgame (leftover tiles are deducted once the bag is
// empty). Each is tuned below in score-point units.
//
// The units are score points on the active letter table, so the whole thing
// stays meaningful even after the distribution has been edited.
// Mean tile value on the active table, so "a high card" means the same thing
// whatever the distribution has been edited to. Recomputed once per search
// rather than per candidate — answerPower() runs for every move the shortlist
// keeps, and this would otherwise be a loop over the whole table each time.
int g_meanVal = 3;

void computeMeanVal(uint8_t lang) {
  int sum = 0, nTiles = 0;
  for (uint8_t i = 0; i < g_dist.count(lang); i++) {
    const TileDef &d = g_dist.at(lang, i);
    if (d.letter == TILE_BLANK) continue;
    sum += (int)d.value * d.count; nTiles += d.count;
  }
  g_meanVal = nTiles ? sum / nTiles : 3;
}

int answerPower(Game &g, const CpuMove &m) {
  const uint8_t lang = g.lang();
  int p = m.score;

  const int meanVal = g_meanVal;

  // Board development: interlocking and opening ground both keep the board
  // growing like a crossword rather than a stack of parallel bars.
  p += 7 * (int)m.crossings;
  p += 3 * (int)m.onEmpty;

  if (m.sOnEnd) p -= 12;

  // --- the leave -------------------------------------------------------
  bool endgame = (g.bagCount() < RACK_N);

  if (m.nleave == 0) {
    p += endgame ? 60 : 12;              // going out ends the game on your terms
    return p;
  }

  int vowels = 0, cons = 0, blanks = 0, heavy = 0, dup = 0;
  bool hasQ = false, hasU = false;
  int leaveVal = 0;
  for (uint8_t i = 0; i < m.nleave; i++) {
    uint8_t t = m.leave[i];
    if (t == TILE_BLANK) { blanks++; continue; }
    if (Game::isVowel(t)) vowels++; else cons++;
    if (t == 'Q') hasQ = true;
    if (t == 'U') hasU = true;
    int v = g_dist.valueOf(lang, t);
    leaveVal += v;
    if (v > meanVal) heavy++;
    for (uint8_t j = 0; j < i; j++) if (m.leave[j] == t) { dup++; break; }
  }

  p += 10 * blanks;                      // a held blank is worth a lot next turn
  p -= 2 * heavy;                        // high cards clog the rack
  p -= 3 * dup;                          // duplicates limit what you can spell
  if (hasQ && !hasU) p -= 10;            // a Q with no U is close to dead

  // Balance: roughly two vowels to five tiles reads best. Penalise the distance
  // from that, in either direction — all-consonant is as bad as all-vowel.
  int want = (vowels + cons + 1) * 2 / 5;
  int off = vowels - want;
  p -= 4 * (off < 0 ? -off : off);

  // Endgame: unplayed tiles are deducted from your score, so holding a heavy
  // rack while the bag is empty is a real loss, not just awkward.
  if (endgame) p -= leaveVal;

  return p;
}

// Record a completed word if it beats anything already kept. The candidate is
// currently sitting in the game as pending tiles, so the engine can score it.
void record(Search &s) {
  if (s.g->pendingCount() == 0) return;
  // The generator must obey the same cross-word limit the validator enforces,
  // or the opponent would pick a move that commit() then refuses and the turn
  // would stall.
  if (s.g->pendingCrossWords() > s.g->crossLimit()) return;
  int sc = s.g->scorePending();
  uint8_t ax = s.horiz ? 0 : 1;
  s.found[ax]++;

  CpuMove *B = s.best[ax];
  uint8_t &N = s.nbest[ax];

  // Insertion sort into this axis's fixed-size descending top-N. The shortlist
  // is kept by SCORE and only re-ranked by power afterwards: power needs the
  // tiles to still be down, so it can't be the gate that decides whether to keep
  // a candidate in the first place without evaluating every one of them.
  uint8_t pos = N;
  while (pos > 0 && B[pos - 1].score < sc) pos--;
  if (pos >= KEEP_BEST) return;

  // Final vet, with the SAME validator commit() uses.
  //
  // The generator is supposed to make only legal words by construction — it
  // walks the DAWG and gates every square on a cross-check. Trusting that was a
  // mistake: Master was showing suggestions that were not words, and no amount
  // of reading the traversal found where. So the shortlist is now verified
  // rather than assumed. This runs only for candidates that would actually
  // enter the top-N, so it costs a handful of lookups per kept move, not one
  // per candidate examined.
  //
  // It also subsumes the cross-word limit and the Dictionary Editor's block
  // list, both of which validate() already enforces.
  {
    String bad;
    MoveErr e = s.g->validate(&bad);
    if (e != MV_OK) {
      // Loud on the one failure that should be impossible, so the underlying
      // generator bug can still be traced. Capped so a bad board cannot flood
      // the log and slow the search further.
      if (e == MV_BAD_WORD && s.rejected < 8) {
        s.rejected++;
        Serial.printf("[CPU] generator produced a non-word: %s\n", bad.c_str());
      }
      return;
    }
  }

  uint8_t last = (N < KEEP_BEST) ? N : KEEP_BEST - 1;
  for (uint8_t i = last; i > pos; i--) B[i] = B[i - 1];

  CpuMove &m = B[pos];
  m.n = s.g->pendingCount();
  m.score = sc;
  m.crossings = s.g->pendingCrossings();
  m.horiz = s.horiz;
  const Pending *p = s.g->pending();
  for (uint8_t i = 0; i < m.n; i++)
    m.p[i] = { p[i].row, p[i].col, p[i].rackIdx, p[i].letter, p[i].isBlank };

  // Must happen here, with the tiles still down: the leave and the word length
  // are not recoverable from the placements once they have been taken back.
  gatherInfo(s, m);
  m.power = answerPower(*s.g, m);

  if (N < KEEP_BEST) N++;
}

// Lay tiles rightward from `off`, following DAWG edges. `node` is the DAWG node
// reached by everything placed/consumed so far.
void extendRight(Search &s, uint32_t node, int off, bool placedAny) {
  if (s.expired) return;
  if (millis() > s.deadline || abortRequested()) { s.expired = true; return; }

  // Off the end of the line: a word can terminate here only if the last edge
  // taken was final, which the caller checked before recursing.
  if (off >= BOARD_N) return;

  uint8_t r, c; mapSq(s.horiz, s.line, off, r, c);

  if (!s.g->isEmpty(r, c)) {
    // Existing tile: the only continuation is its own letter.
    uint8_t letter = s.g->at(r, c);
    int8_t li = s.d->indexOf(letter);
    if (li < 0) return;
    uint32_t e;
    if (!s.d->findEdge(node, (uint8_t)li, e)) return;
    if (Dawg::endOfWord(e) && placedAny && !occupied(*s.g, s.horiz, s.line, off + 1))
      record(s);
    uint32_t t = Dawg::target(e);
    if (t) extendRight(s, t, off + 1, placedAny);
    return;
  }

  // Empty square: try every DAWG edge the cross-check and the rack allow.
  for (uint32_t i = node; i < s.d->edgeCount(); i++) {
    uint32_t e = s.d->edge(i);
    uint8_t  li = Dawg::letterIdx(e);
    bool     lastEdge = Dawg::lastEdge(e);

    if (s.cross[off] & (1UL << li)) {
      uint8_t letter = s.d->letterByte(li);

      for (uint8_t k = 0; k < RACK_N; k++) {
        if (s.used[k]) continue;
        uint8_t t = s.g->player(s.g->current()).rack[k];
        if (t == TILE_EMPTY) continue;
        bool asBlank = (t == TILE_BLANK);
        if (!asBlank && t != letter) continue;

        // Two identical tiles in the rack would explore an identical subtree,
        // so only the first unused one of each kind is tried.
        bool seen = false;
        for (uint8_t j = 0; j < k; j++)
          if (!s.used[j] && s.g->player(s.g->current()).rack[j] == t) { seen = true; break; }
        if (seen) continue;

        if (!s.g->place(r, c, k, asBlank ? letter : 0)) continue;
        s.used[k] = true;

        if (Dawg::endOfWord(e) && !occupied(*s.g, s.horiz, s.line, off + 1))
          record(s);
        uint32_t tgt = Dawg::target(e);
        if (tgt) extendRight(s, tgt, off + 1, true);

        s.used[k] = false;
        s.g->unplaceAt(r, c);
        if (s.expired) return;
      }
    }
    if (lastEdge) break;
  }
}

// Build a prefix of EXACTLY `len` rack tiles immediately left of the anchor,
// then hand off to extendRight at the anchor itself.
//
// `depth` is how much of that prefix is already down, so the DAWG's depth-th
// letter lands on square `anchorOff - len + depth` — left to right, in the same
// order the DAWG walked them.
//
// That ordering is the whole point. This used to place the prefix growing
// LEFTWARD from the anchor (`anchorOff - leftLen - 1`), which put the DAWG's
// first letter nearest the anchor and its last letter furthest away: the word
// was laid on the board REVERSED. A one-tile prefix reads the same either way,
// which is why the bug only surfaced on prefixes of two or more and looked like
// the generator was suggesting the rack in scrambled order — it was.
//
// Fixing the length up front, rather than growing it, is what lets the square
// be computed before the prefix is complete.
void leftPart(Search &s, uint32_t node, int anchorOff, uint8_t len, uint8_t depth) {
  if (s.expired) return;
  if (depth == len) { extendRight(s, node, anchorOff, len > 0); return; }

  int pos = anchorOff - len + depth;
  if (pos < 0) return;
  uint8_t r, c; mapSq(s.horiz, s.line, pos, r, c);
  if (!s.g->isEmpty(r, c)) return;

  for (uint32_t i = node; i < s.d->edgeCount(); i++) {
    uint32_t e = s.d->edge(i);
    uint8_t  li = Dawg::letterIdx(e);
    bool     lastEdge = Dawg::lastEdge(e);

    if (s.cross[pos] & (1UL << li)) {
      uint8_t letter = s.d->letterByte(li);
      uint32_t tgt = Dawg::target(e);
      if (tgt) {
        for (uint8_t k = 0; k < RACK_N; k++) {
          if (s.used[k]) continue;
          uint8_t t = s.g->player(s.g->current()).rack[k];
          if (t == TILE_EMPTY) continue;
          bool asBlank = (t == TILE_BLANK);
          if (!asBlank && t != letter) continue;

          bool seen = false;
          for (uint8_t j = 0; j < k; j++)
            if (!s.used[j] && s.g->player(s.g->current()).rack[j] == t) { seen = true; break; }
          if (seen) continue;

          if (!s.g->place(r, c, k, asBlank ? letter : 0)) continue;
          s.used[k] = true;
          leftPart(s, tgt, anchorOff, len, depth + 1);
          s.used[k] = false;
          s.g->unplaceAt(r, c);
          if (s.expired) return;
        }
      }
    }
    if (lastEdge) break;
  }
}

// Allowed letters at each empty square of this line, judged on the cross axis.
// A square with no perpendicular neighbours accepts anything.
//
// Note this uses the SAME dictionary as the main search (s.d). When the CPU is
// on the reduced everyday list that means every incidental cross-word it forms
// must also be an everyday word — not just the word it set out to play. The
// prefixes it walks are likewise prefixes of everyday words, since the whole
// traversal runs on that one DAWG.
void buildCrossChecks(Search &s) {
  const uint32_t ALL = (s.d->alphaSize() >= 32) ? 0xFFFFFFFFUL
                                                : ((1UL << s.d->alphaSize()) - 1);
  for (int off = 0; off < BOARD_N; off++) {
    uint8_t r, c; mapSq(s.horiz, s.line, off, r, c);
    s.anchor[off] = false;
    s.cross[off] = 0;
    if (!s.g->isEmpty(r, c)) continue;

    // Anchor = empty square touching a placed tile on any side.
    const int8_t dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
    for (uint8_t k = 0; k < 4; k++) {
      int ar = (int)r + dr[k], ac = (int)c + dc[k];
      if (ar < 0 || ac < 0 || ar >= BOARD_N || ac >= BOARD_N) continue;
      if (!s.g->isEmpty((uint8_t)ar, (uint8_t)ac)) { s.anchor[off] = true; break; }
    }

    // Collect the perpendicular prefix above/left and suffix below/right.
    uint8_t pre[BOARD_N], nPre = 0, suf[BOARD_N], nSuf = 0;
    int8_t pdr = s.horiz ? -1 : 0, pdc = s.horiz ? 0 : -1;   // perpendicular step
    {
      int rr = (int)r + pdr, cc = (int)c + pdc;
      while (rr >= 0 && cc >= 0 && rr < BOARD_N && cc < BOARD_N &&
             !s.g->isEmpty((uint8_t)rr, (uint8_t)cc)) {
        pre[nPre++] = s.g->at((uint8_t)rr, (uint8_t)cc);
        rr += pdr; cc += pdc;
      }
      // walked backwards — reverse into reading order
      for (uint8_t i = 0; i < nPre / 2; i++) {
        uint8_t t = pre[i]; pre[i] = pre[nPre - 1 - i]; pre[nPre - 1 - i] = t;
      }
      rr = (int)r - pdr; cc = (int)c - pdc;
      while (rr >= 0 && cc >= 0 && rr < BOARD_N && cc < BOARD_N &&
             !s.g->isEmpty((uint8_t)rr, (uint8_t)cc)) {
        suf[nSuf++] = s.g->at((uint8_t)rr, (uint8_t)cc);
        rr -= pdr; cc -= pdc;
      }
    }

    if (nPre == 0 && nSuf == 0) { s.cross[off] = ALL; continue; }

    uint8_t w[BOARD_N * 2];
    for (uint8_t li = 0; li < s.d->alphaSize(); li++) {
      uint8_t n = 0;
      for (uint8_t i = 0; i < nPre; i++) w[n++] = pre[i];
      w[n++] = s.d->letterByte(li);
      for (uint8_t i = 0; i < nSuf; i++) w[n++] = suf[i];
      // Overrides apply here too, so the opponent never builds a play whose
      // cross-word you have blocked. (Allowed words it still cannot FIND — the
      // search walks the DAWG. See wordedit.h.)
      if (wordAllowed(s.g->lang(), w, n, s.d->contains(w, n)))
        s.cross[off] |= (1UL << li);
    }
  }
}

// Rank indices of `best` by a caller-supplied trait, descending.
template <typename F>
void rankBy(const CpuMove *best, uint8_t n, uint8_t *ord, F trait) {
  for (uint8_t i = 0; i < n; i++) ord[i] = i;
  for (uint8_t i = 1; i < n; i++) {                 // insertion sort, n <= 32
    uint8_t k = ord[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && trait(best[ord[j]]) < trait(best[k])) { ord[j + 1] = ord[j]; j--; }
    ord[j + 1] = k;
  }
}

// Which move the opponent actually plays.
//
// Rather than always taking the best move and adding noise, it ranks the
// candidates and picks from a RANGE whose width is the difficulty, and with some
// probability re-ranks by a single trait first (word length, tiles laid, ground
// opened, or points per tile). The result is an opponent that appears to have
// habits — a taste for long words one game, for spreading out the next — rather
// than one fixed style plus jitter.
//
// Ranking is by answerPower(), not raw score, so the leave and board shape count.
uint8_t cpuPick(const CpuMove *best, uint8_t nbest, uint8_t level) {
  if (nbest == 0) return 0;

  uint8_t ord[KEEP_BEST * 2];

  // Incentive probability falls as the opponent gets stronger: a hard opponent
  // mostly just plays well.
  const uint8_t incPct = (level >= 2) ? 10 : (level == 1) ? 25 : 35;

  if ((uint8_t)random(100) < incPct) {
    switch (random(4)) {
      case 0:  rankBy(best, nbest, ord, [](const CpuMove &m) { return (int)m.wordLen; }); break;
      case 1:  rankBy(best, nbest, ord, [](const CpuMove &m) { return (int)m.n; }); break;
      case 2:  rankBy(best, nbest, ord, [](const CpuMove &m) { return (int)m.onEmpty; }); break;
      // LaidMult: no premium-square flag is carried on a move, but points per
      // tile laid is a good proxy — a multiplier is what makes it high.
      default: rankBy(best, nbest, ord, [](const CpuMove &m) {
                 return m.n ? m.score / (int)m.n : 0; }); break;
    }
  } else {
    rankBy(best, nbest, ord, [](const CpuMove &m) { return m.power; });
  }

  // The range: how far down the ranking the opponent is willing to look.
  if (level >= 2) {
    uint8_t ties = 1;
    while (ties < nbest && best[ord[ties]].power == best[ord[0]].power) ties++;
    return ord[random(ties)];
  }
  if (level == 1) {
    uint8_t top = (nbest < 4) ? nbest : 4;
    return ord[random(top)];
  }
  uint8_t lo = nbest / 2;
  return ord[lo + random(nbest - lo)];
}

// Interleave the two per-orientation lists into one ranked array. Taking them
// alternately while both have entries guarantees neither axis can be squeezed
// out by the other, whatever the raw scores look like.
uint8_t mergeBest(Search &s, CpuMove *out, uint8_t maxN) {
  uint8_t a = 0, b = 0, n = 0;
  while (n < maxN && (a < s.nbest[0] || b < s.nbest[1])) {
    bool takeA;
    if      (a >= s.nbest[0]) takeA = false;
    else if (b >= s.nbest[1]) takeA = true;
    else    takeA = (s.best[0][a].score >= s.best[1][b].score);
    out[n++] = takeA ? s.best[0][a++] : s.best[1][b++];
  }
  return n;
}

} // namespace

// Shared search body. Fills s.best / s.nbest; the two public entry points differ
// only in how they read that ranked list.
static bool runSearch(Game &g, const Dawg &dict, Search &s, uint32_t budgetMs) {
  if (!dict.loaded()) return false;

  s.g = &g;
  s.d = &dict;
  g_aborted = false;
  s.nbest[0] = s.nbest[1] = 0;
  s.found[0] = s.found[1] = 0;
  s.starved[0] = s.starved[1] = false;
  s.expired = false;
  s.rejected = 0;

  g.clearPending();
  computeMeanVal(g.lang());

  // Which orientation is searched FIRST is randomised. It used to always be
  // horizontal, which biased play three ways: the time budget could expire
  // before the vertical pass ran at all, equal-scoring moves resolved to
  // whichever was found first, and the opening move was always horizontal.
  bool firstHoriz = (random(2) == 0);

  // Each orientation gets HALF the budget, with its own deadline. Sharing one
  // deadline meant a slow first orientation could consume all of it and the
  // second was never searched at all — which is what made the opponent look
  // like it only ever played horizontally. Randomising the order alone did not
  // fix that; it only changed which orientation got starved.
  for (uint8_t dir = 0; dir < 2 && !g_aborted; dir++) {
    s.horiz = (dir == 0) ? firstHoriz : !firstHoriz;
    s.deadline = millis() + budgetMs / 2;
    s.expired = false;

    for (uint8_t line = 0; line < BOARD_N && !s.expired; line++) {
      s.line = line;
      buildCrossChecks(s);

      for (int off = 0; off < BOARD_N && !s.expired; off++) {
        // On an empty board nothing is adjacent to anything, so the centre star
        // is the only anchor. Both orientations are still searched through it.
        bool isAnchor = !g.firstMovePlayed()
                          ? (line == BOARD_N / 2 && off == BOARD_N / 2)
                          : s.anchor[off];
        if (!isAnchor) continue;

        memset(s.used, 0, sizeof(s.used));

        if (occupied(g, s.horiz, line, off - 1)) {
          // Tiles already sit to the left: that prefix is fixed. Walk it through
          // the DAWG, then extend right from the anchor.
          int start = off - 1;
          while (occupied(g, s.horiz, line, start - 1)) start--;
          uint32_t node = dict.root();
          bool ok = true;
          for (int p = start; p < off && ok; p++) {
            int8_t li = dict.indexOf(letterAt(g, s.horiz, line, p));
            uint32_t e;
            if (li < 0 || !dict.findEdge(node, (uint8_t)li, e)) { ok = false; break; }
            node = Dawg::target(e);
            if (!node) ok = false;
          }
          if (ok) extendRight(s, node, off, false);
        } else {
          // Count how many empty non-anchor squares are free to the left.
          uint8_t limit = 0;
          for (int p = off - 1; p >= 0; p--) {
            if (occupied(g, s.horiz, line, p)) break;
            if (s.anchor[p]) break;          // that square is another anchor's job
            limit++;
            if (limit >= RACK_N - 1) break;
          }
          // One pass per prefix length, since the length has to be known before
          // the first tile can be given its square.
          for (uint8_t L = 0; L <= limit && !s.expired; L++)
            leftPart(s, dict.root(), off, L, 0);
        }
      }
    }
    s.starved[s.horiz ? 0 : 1] = s.expired;
  }

  g.clearPending();
  // One line per search. If one axis reports far fewer candidates, or reports
  // (BUDGET), that is where a play-style bias is coming from.
  Serial.printf("[CPU] H %u found/%u kept%s   V %u found/%u kept%s\n",
                s.found[0], s.nbest[0], s.starved[0] ? " BUDGET" : "",
                s.found[1], s.nbest[1], s.starved[1] ? " BUDGET" : "");
  return (s.nbest[0] + s.nbest[1]) > 0;
}

// The Search block holds two top-16 lists plus scratch; keeping it off the stack
// leaves headroom for the recursive generator.
static Search g_search;

// True if the wider list is worth a second pass.
static bool haveFallback(const Dawg &dict, const Dawg *fallback) {
  return fallback && fallback != &dict && fallback->loaded();
}

bool cpuFindMove(Game &g, const Dawg &dict, uint8_t level, CpuMove &out,
                 uint32_t budgetMs, const Dawg *fallback) {
  static CpuMove merged[KEEP_BEST * 2];
  uint8_t n = 0;
  if (runSearch(g, dict, g_search, budgetMs))
    n = mergeBest(g_search, merged, KEEP_BEST * 2);

  // Nothing on the everyday list. Rather than skip a turn, look again at the
  // full one — an obscure short word is a better move than no move.
  if (!n && !cpuWasAborted() && haveFallback(dict, fallback)) {
    Serial.println(F("[CPU] no move on the everyday list; retrying on the full one"));
    if (runSearch(g, *fallback, g_search, budgetMs))
      n = mergeBest(g_search, merged, KEEP_BEST * 2);
  }
  if (!n) return false;
  out = merged[cpuPick(merged, n, level)];
  return true;
}

uint8_t cpuFindMoves(Game &g, const Dawg &dict, CpuMove *out, uint8_t maxN,
                     uint32_t budgetMs, const Dawg *fallback) {
  static CpuMove merged[KEEP_BEST * 2];
  uint8_t n = 0;
  if (runSearch(g, dict, g_search, budgetMs))
    n = mergeBest(g_search, merged, KEEP_BEST * 2);
  if (!n && !cpuWasAborted() && haveFallback(dict, fallback)) {
    if (runSearch(g, *fallback, g_search, budgetMs))
      n = mergeBest(g_search, merged, KEEP_BEST * 2);
  }
  if (!n) return 0;

  // Master orders its suggestions by how good the move is to play, not by how
  // many points it scores. A slightly cheaper word that leaves a workable rack
  // belongs above a rack-dumping one.
  uint8_t ord[KEEP_BEST * 2];
  rankBy(merged, n, ord, [](const CpuMove &m) { return m.power; });

  if (n > maxN) n = maxN;
  for (uint8_t i = 0; i < n; i++) out[i] = merged[ord[i]];
  return n;
}

void cpuApply(Game &g, const CpuMove &m) {
  g.clearPending();
  for (uint8_t i = 0; i < m.n; i++)
    g.place(m.p[i].row, m.p[i].col, m.p[i].rackIdx,
            m.p[i].isBlank ? m.p[i].letter : 0);
}
