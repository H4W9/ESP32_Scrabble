// game.cpp — see game.h for the model.

#include "game.h"
#include "letters.h"
#include "wordedit.h"
#include <SD.h>
#include <esp_random.h>          // esp_random() for the per-game reseed in begin()

// Tile bags now live in letters.cpp as an EDITABLE distribution (Settings ->
// Letters), so the engine reads whatever the player has configured rather than
// a compiled-in table. The built-in defaults and their provenance are documented
// there.

uint8_t Game::letterValue(uint8_t letter) const {
  return g_dist.valueOf(_lang, letter);
}

void Game::fillBag() {
  _bagN = 0;
  for (uint8_t i = 0; i < g_dist.count(_lang); i++) {
    const TileDef &d = g_dist.at(_lang, i);
    for (uint8_t k = 0; k < d.count && _bagN < MAX_BAG; k++)
      _bag[_bagN++] = d.letter;
  }
}

bool Game::isVowel(uint8_t t) {
  return t == 'A' || t == 'E' || t == 'I' || t == 'O' || t == 'U' ||
         t == 0x80 || t == 0x82 || t == 0x84;      // Ae Oe Ue
}

// Fast PRNG (xorshift32) for the shuffle, seeded ONCE per shuffle from the
// hardware RNG.
//
// The bag shuffle has now been wrong twice, and the reason both times was the
// randomness source, not the Fisher-Yates:
//   1. An "intercalating" shuffle made the bag's draw end uniform, so the luck
//      helper dealt the same opening rack every game whatever the seed.
//   2. A plain Fisher-Yates that called esp_random() PER SWAP still repeated,
//      because on this chip esp_random() returns the same value when polled in a
//      tight loop before the radio is up -- freezing it into one permutation.
// So esp_random() is read exactly ONCE here to inject entropy, mixed with
// micros() (which drifts with how long the player spent in the menus) and with
// the state carried from the previous shuffle, then a cheap software PRNG does
// the per-swap work. That gives a genuinely different bag every game.
static uint32_t g_rngState = 0x9E3779B9u;
static inline uint32_t rngNext() {
  uint32_t s = g_rngState;
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  g_rngState = s ? s : 0x9E3779B9u;         // xorshift must never sit at 0
  return g_rngState;
}

void Game::shuffleBag() {
  g_rngState ^= esp_random() ^ (uint32_t)micros();
  if (g_rngState == 0) g_rngState = 0x9E3779B9u;
  for (int i = _bagN - 1; i > 0; i--) {
    int j = (int)(rngNext() % (uint32_t)(i + 1));
    uint8_t t = _bag[i]; _bag[i] = _bag[j]; _bag[j] = t;
  }
}

bool Game::drawTile(uint8_t &out) {
  if (_bagN == 0) return false;
  out = _bag[--_bagN];
  return true;
}

// One tile off the top of the bag, preferring one that satisfies `want`.
// Scans down from the top for the nearest acceptable tile and lifts it out; if
// nothing in the bag qualifies, takes the top tile anyway. `want` (vowel /
// consonant / either), the no-blank rule and the repeat-avoidance are all a
// preference, never a hard requirement, so this can't fail or loop.
bool Game::drawTilePreferring(uint8_t &out, int want, const uint8_t *rack) {
  if (_bagN == 0) return false;
  int pick = -1;
  for (int i = _bagN - 1; i >= 0 && pick < 0; i--) {
    uint8_t t = _bag[i];
    if (t == TILE_BLANK) continue;                 // never spend luck on a blank
    if (want > 0 && !isVowel(t)) continue;
    if (want < 0 && isVowel(t)) continue;
    if (rack) {                                    // avoid a third of the same letter
      uint8_t same = 0;
      for (uint8_t k = 0; k < RACK_N; k++) if (rack[k] == t) same++;
      if (same >= 2) continue;
    }
    pick = i;
  }
  if (pick < 0) { out = _bag[--_bagN]; return true; }
  out = _bag[pick];
  for (int i = pick; i < (int)_bagN - 1; i++) _bag[i] = _bag[i + 1];
  _bagN--;
  return true;
}

// Top the rack back up to seven.
//
// With the luck helper on, as each slot is filled the rack so far decides
// whether a vowel or a consonant is wanted, so a hand can't come out all one or
// the other. The target is two vowels in
// seven; the helper only ever expresses a preference, so an exhausted bag
// degrades to an ordinary draw rather than stalling.
void Game::refill(uint8_t p) {
  uint8_t *rack = _players[p].rack;
  for (uint8_t i = 0; i < RACK_N; i++) {
    if (rack[i] != TILE_EMPTY) continue;
    if (!_luckHelper) {
      if (!drawTile(rack[i])) rack[i] = TILE_EMPTY;
      continue;
    }
    uint8_t held = 0, vowels = 0;
    for (uint8_t k = 0; k < RACK_N; k++) {
      if (rack[k] == TILE_EMPTY || rack[k] == TILE_BLANK) continue;
      held++; if (isVowel(rack[k])) vowels++;
    }
    // Slots still to fill after this one, so the balance is judged against the
    // finished hand rather than the partial one.
    int want = 0;
    int wantVowels = 2;
    if (vowels < wantVowels && (int)(RACK_N - held) <= (wantVowels - (int)vowels)) want = 1;
    else if (vowels >= wantVowels + 1) want = -1;

    if (!drawTilePreferring(rack[i], want, rack)) rack[i] = TILE_EMPTY;
  }
}

void Game::begin(const Dawg *dict, uint8_t lang, uint8_t numPlayers) {
  // Reseed here, per new game, rather than trusting the boot seed. esp_random()
  // is only well-seeded once the RF subsystem is up (at boot it can return a
  // fixed value, and randomSeed ignores a 0 seed), which made every fresh boot
  // deal the same bag. By the time a game starts the radio is up and micros()
  // has drifted with the player's menu navigation, so the two together give a
  // fresh, non-zero seed every game.
  randomSeed((uint32_t)esp_random() ^ (uint32_t)micros() ^ 1u);

  _dict = dict;
  _lang = lang;
  _nplayers = constrain(numPlayers, 2, MAX_PLAYERS);
  memset(_board, TILE_EMPTY, sizeof(_board));
  memset(_blank, 0, sizeof(_blank));
  _anyPlayed = false;
  _turn = 0;
  _pendN = 0;
  _lastN = 0;
  _histN = 0;
  _passStreak = 0;
  _crossLimit = MAX_CROSS_WORDS;
  fillBag();
  shuffleBag();
  // Initialise every slot, not just the ones in play: setPlayer() is optional
  // from the caller's side, and an uninitialised isCpu would hand a human's
  // turn to the (M3) move generator.
  for (uint8_t p = 0; p < MAX_PLAYERS; p++) {
    _players[p].score = 0;
    _players[p].isCpu = false;
    _players[p].cpuLevel = 1;
    snprintf(_players[p].name, sizeof(_players[p].name), "Player %u", (unsigned)(p + 1));
    memset(_players[p].rack, TILE_EMPTY, RACK_N);
  }
  for (uint8_t p = 0; p < _nplayers; p++) refill(p);

  // Diagnostic: the opening rack should differ every new game. If this line
  // shows the SAME letters game after game, the shuffle RNG is not varying (read
  // the rngState value); if the rack varies here but the screen does not, the
  // build on the device is stale. Safe to delete once confirmed working.
  Serial.printf("[Deal] rng=%08X rack=", (unsigned)g_rngState);
  for (uint8_t i = 0; i < RACK_N; i++) Serial.printf("%c", _players[0].rack[i]);
  Serial.println();
}

void Game::setPlayer(uint8_t i, const char *name, bool isCpu, uint8_t cpuLevel) {
  if (i >= MAX_PLAYERS) return;
  strncpy(_players[i].name, name, sizeof(_players[i].name) - 1);
  _players[i].name[sizeof(_players[i].name) - 1] = 0;
  _players[i].isCpu = isCpu;
  _players[i].cpuLevel = cpuLevel;
}

// Pending placement
bool Game::isPendingAt(uint8_t r, uint8_t c) const {
  for (uint8_t i = 0; i < _pendN; i++) if (_pend[i].row == r && _pend[i].col == c) return true;
  return false;
}

uint8_t Game::shownAt(uint8_t r, uint8_t c) const {
  for (uint8_t i = 0; i < _pendN; i++) if (_pend[i].row == r && _pend[i].col == c) return _pend[i].letter;
  return _board[r][c];
}

bool Game::place(uint8_t r, uint8_t c, uint8_t rackIdx, uint8_t declaredLetter) {
  if (r >= BOARD_N || c >= BOARD_N || rackIdx >= RACK_N) return false;
  if (_pendN >= RACK_N) return false;
  if (_board[r][c] != TILE_EMPTY || isPendingAt(r, c)) return false;
  uint8_t t = _players[_turn].rack[rackIdx];
  if (t == TILE_EMPTY) return false;
  // That rack slot may already be on the board this turn.
  for (uint8_t i = 0; i < _pendN; i++) if (_pend[i].rackIdx == rackIdx) return false;

  bool blank = (t == TILE_BLANK);
  _pend[_pendN++] = { r, c, blank ? declaredLetter : t, rackIdx, blank };
  return true;
}

bool Game::unplaceAt(uint8_t r, uint8_t c) {
  for (uint8_t i = 0; i < _pendN; i++) {
    if (_pend[i].row == r && _pend[i].col == c) {
      for (uint8_t j = i; j + 1 < _pendN; j++) _pend[j] = _pend[j + 1];
      _pendN--;
      return true;
    }
  }
  return false;
}

void Game::clearPending() { _pendN = 0; }

void Game::moveRackTile(uint8_t from, uint8_t to) {
  if (from >= RACK_N || to >= RACK_N || from == to) return;
  uint8_t *rk = _players[_turn].rack;
  uint8_t  t  = rk[from];

  // Slide everything between the two slots, then drop the tile in. Build the
  // old->new index map as we go so pending tiles can follow their slot.
  uint8_t map[RACK_N];
  for (uint8_t i = 0; i < RACK_N; i++) map[i] = i;

  if (from < to) {
    for (uint8_t i = from; i < to; i++) { rk[i] = rk[i + 1]; map[i + 1] = i; }
  } else {
    for (uint8_t i = from; i > to; i--) { rk[i] = rk[i - 1]; map[i - 1] = i; }
  }
  rk[to] = t;
  map[from] = to;

  for (uint8_t i = 0; i < _pendN; i++) _pend[i].rackIdx = map[_pend[i].rackIdx];
}

bool Game::wasLastMove(uint8_t r, uint8_t c) const {
  for (uint8_t i = 0; i < _lastN; i++) if (_lastR[i] == r && _lastC[i] == c) return true;
  return false;
}

// Word reading + validation
// Walk back to the start of the word through (r,c) along the axis, then read
// forward collecting letters and their squares. Counts pending tiles, so it
// describes the board as it would be if the move were committed.
uint8_t Game::readWord(uint8_t r, uint8_t c, int8_t dr, int8_t dc,
                       uint8_t *out, uint8_t *rows, uint8_t *cols) const {
  int sr = r, sc = c;
  while (true) {
    int pr = sr - dr, pc = sc - dc;
    if (pr < 0 || pc < 0 || pr >= BOARD_N || pc >= BOARD_N) break;
    if (shownAt(pr, pc) == TILE_EMPTY) break;
    sr = pr; sc = pc;
  }
  uint8_t n = 0;
  while (sr >= 0 && sc >= 0 && sr < BOARD_N && sc < BOARD_N) {
    uint8_t l = shownAt(sr, sc);
    if (l == TILE_EMPTY) break;
    out[n] = l; rows[n] = sr; cols[n] = sc;
    n++;
    sr += dr; sc += dc;
  }
  return n;
}

bool Game::wordOk(const uint8_t *w, uint8_t len) const {
  if (len < 2) return true;              // a single letter is not a word claim
  // The Dictionary Editor's overrides sit on top of the list, so a word you
  // blocked is refused and a word you allowed is accepted.
  return wordAllowed(_lang, w, len, _dict && _dict->contains(w, len));
}

MoveErr Game::validate(String *badWord) const {
  if (_pendN == 0) return MV_NO_TILES;

  // All placed tiles must share a row or a column.
  bool sameRow = true, sameCol = true;
  for (uint8_t i = 1; i < _pendN; i++) {
    if (_pend[i].row != _pend[0].row) sameRow = false;
    if (_pend[i].col != _pend[0].col) sameCol = false;
  }
  if (!sameRow && !sameCol) return MV_NOT_IN_LINE;

  // A single tile could extend either way; treat it as horizontal and let the
  // cross-word check cover the vertical.
  int8_t dr = sameRow ? 0 : 1, dc = sameRow ? 1 : 0;

  // No gaps: between the extreme placed tiles every square must be filled.
  {
    uint8_t lo = 0xFF, hi = 0;
    for (uint8_t i = 0; i < _pendN; i++) {
      uint8_t v = sameRow ? _pend[i].col : _pend[i].row;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    uint8_t fixed = sameRow ? _pend[0].row : _pend[0].col;
    for (uint8_t v = lo; v <= hi; v++) {
      uint8_t rr = sameRow ? fixed : v, cc = sameRow ? v : fixed;
      if (shownAt(rr, cc) == TILE_EMPTY) return MV_HAS_GAP;
    }
  }

  // Opening move must cover the centre star; later moves must touch the board.
  if (!_anyPlayed) {
    bool onCentre = false;
    for (uint8_t i = 0; i < _pendN; i++)
      if (_pend[i].row == BOARD_N / 2 && _pend[i].col == BOARD_N / 2) onCentre = true;
    if (!onCentre) return MV_NOT_CENTRE;
  } else {
    bool touches = false;
    for (uint8_t i = 0; i < _pendN && !touches; i++) {
      int r = _pend[i].row, c = _pend[i].col;
      const int8_t nr[4] = { -1, 1, 0, 0 }, nc[4] = { 0, 0, -1, 1 };
      for (uint8_t k = 0; k < 4; k++) {
        int ar = r + nr[k], ac = c + nc[k];
        if (ar < 0 || ac < 0 || ar >= BOARD_N || ac >= BOARD_N) continue;
        if (_board[ar][ac] != TILE_EMPTY) { touches = true; break; }
      }
    }
    if (!touches) return MV_NOT_CONNECTED;
  }

  uint8_t w[BOARD_N], rs[BOARD_N], cs[BOARD_N];
  bool formedAny = false;      // did this placement make a word of 2+ letters?

  // The main word along the axis of play.
  uint8_t n = readWord(_pend[0].row, _pend[0].col, dr, dc, w, rs, cs);
  if (n >= 2) formedAny = true;
  if (!wordOk(w, n)) {
    if (badWord) { *badWord = ""; for (uint8_t i = 0; i < n; i++) *badWord += (char)w[i]; }
    return MV_BAD_WORD;
  }

  // Every cross word formed by a newly placed tile, on the perpendicular axis.
  for (uint8_t i = 0; i < _pendN; i++) {
    uint8_t m = readWord(_pend[i].row, _pend[i].col, dc, dr, w, rs, cs);
    if (m >= 2) formedAny = true;
    if (!wordOk(w, m)) {
      if (badWord) { *badWord = ""; for (uint8_t k = 0; k < m; k++) *badWord += (char)w[k]; }
      return MV_BAD_WORD;
    }
  }
  // A lone tile on the centre star would otherwise validate and score nothing.
  if (!formedAny) return MV_NO_WORD;
  if (pendingCrossWords() > _crossLimit) return MV_TOO_MANY_CROSS;
  return MV_OK;
}

// Scoring
// Premium squares only count when a tile is newly placed on them, so a word
// re-using an existing tile does not re-collect that square's bonus.
int Game::scoreWord(const uint8_t *rows, const uint8_t *cols, uint8_t len) const {
  int sum = 0, wordMul = 1;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t r = rows[i], c = cols[i];
    bool fresh = isPendingAt(r, c);
    uint8_t letter = shownAt(r, c);

    int lv = 0;
    if (fresh) {
      bool blank = false;
      for (uint8_t k = 0; k < _pendN; k++)
        if (_pend[k].row == r && _pend[k].col == c) blank = _pend[k].isBlank;
      lv = blank ? 0 : letterValue(letter);
    } else {
      lv = _blank[r][c] ? 0 : letterValue(letter);
    }

    if (fresh) {
      Premium p = premiumAt(r, c);
      lv *= letterMult(p);
      wordMul *= wordMult(p);
    }
    sum += lv;
  }
  return sum * wordMul;
}

int Game::scorePending(String *wordsOut) const {
  if (_pendN == 0) return 0;
  bool sameRow = true;
  for (uint8_t i = 1; i < _pendN; i++) if (_pend[i].row != _pend[0].row) sameRow = false;
  int8_t dr = sameRow ? 0 : 1, dc = sameRow ? 1 : 0;

  uint8_t w[BOARD_N], rs[BOARD_N], cs[BOARD_N];
  int total = 0;
  if (wordsOut) *wordsOut = "";

  auto addWord = [&](uint8_t n) {
    if (n < 2) return;
    total += scoreWord(rs, cs, n);
    if (wordsOut) {
      if (wordsOut->length()) *wordsOut += ", ";
      for (uint8_t i = 0; i < n; i++) *wordsOut += (char)w[i];
    }
  };

  addWord(readWord(_pend[0].row, _pend[0].col, dr, dc, w, rs, cs));
  for (uint8_t i = 0; i < _pendN; i++)
    addWord(readWord(_pend[i].row, _pend[i].col, dc, dr, w, rs, cs));

  if (_pendN == RACK_N) total += (int)g_dist.bingo(_lang);
  return total;
}

uint8_t Game::pendingCrossings() const {
  if (_pendN == 0) return 0;
  bool sameRow = true;
  for (uint8_t i = 1; i < _pendN; i++) if (_pend[i].row != _pend[0].row) sameRow = false;
  int8_t dr = sameRow ? 0 : 1, dc = sameRow ? 1 : 0;

  uint8_t w[BOARD_N], rs[BOARD_N], cs[BOARD_N];
  uint8_t n = readWord(_pend[0].row, _pend[0].col, dr, dc, w, rs, cs);
  uint8_t out = (n > _pendN) ? (uint8_t)(n - _pendN) : 0;   // board tiles reused
  for (uint8_t i = 0; i < _pendN; i++)                      // perpendicular words
    if (readWord(_pend[i].row, _pend[i].col, dc, dr, w, rs, cs) >= 2) out++;
  return out;
}

uint8_t Game::pendingCrossWords() const {
  if (_pendN == 0) return 0;
  bool sameRow = true;
  for (uint8_t i = 1; i < _pendN; i++) if (_pend[i].row != _pend[0].row) sameRow = false;
  int8_t dr = sameRow ? 0 : 1, dc = sameRow ? 1 : 0;
  uint8_t w[BOARD_N], rs[BOARD_N], cs[BOARD_N], n = 0;
  for (uint8_t i = 0; i < _pendN; i++)
    if (readWord(_pend[i].row, _pend[i].col, dc, dr, w, rs, cs) >= 2) n++;
  return n;
}

void Game::addHistory(uint8_t p, const char *word, int score) {
  if (_histN >= MAX_HISTORY) {            // oldest out; a long game still shows recent play
    for (uint8_t i = 1; i < MAX_HISTORY; i++) _hist[i - 1] = _hist[i];
    _histN = MAX_HISTORY - 1;
  }
  MoveRec &m = _hist[_histN++];
  m.player = p;
  m.score = (int16_t)score;
  strncpy(m.word, word ? word : "", sizeof(m.word) - 1);
  m.word[sizeof(m.word) - 1] = 0;
}

bool Game::commit() {
  if (validate() != MV_OK) return false;

  // scorePending() lists every word the move forms; the history wants the one
  // that was played, which is the first (the word along the axis of play).
  String words;
  int sc = scorePending(&words);
  int comma = words.indexOf(", ");
  String main = (comma < 0) ? words : words.substring(0, comma);
  addHistory(_turn, main.c_str(), sc);
  _players[_turn].score += sc;

  _lastN = 0;
  for (uint8_t i = 0; i < _pendN; i++) {
    const Pending &p = _pend[i];
    _board[p.row][p.col] = p.letter;
    _blank[p.row][p.col] = p.isBlank;
    _players[_turn].rack[p.rackIdx] = TILE_EMPTY;
    if (_lastN < RACK_N) { _lastR[_lastN] = p.row; _lastC[_lastN] = p.col; _lastN++; }
  }
  _pendN = 0;
  _anyPlayed = true;
  _passStreak = 0;
  refill(_turn);
  _turn = (_turn + 1) % _nplayers;
  return true;
}

void Game::pass() {
  _pendN = 0;
  addHistory(_turn, "", 0);
  _passStreak++;
  _turn = (_turn + 1) % _nplayers;
}

bool Game::exchange(const bool *which) {
  if (_bagN < RACK_N) return false;          // not allowed on a nearly-empty bag
  uint8_t back[RACK_N], nb = 0;
  for (uint8_t i = 0; i < RACK_N; i++) {
    if (which[i] && _players[_turn].rack[i] != TILE_EMPTY) {
      back[nb++] = _players[_turn].rack[i];
      _players[_turn].rack[i] = TILE_EMPTY;
    }
  }
  if (nb == 0) return false;
  _pendN = 0;
  refill(_turn);                              // draw replacements FIRST...
  for (uint8_t i = 0; i < nb && _bagN < MAX_BAG; i++) _bag[_bagN++] = back[i];
  shuffleBag();                               // ...then return the old ones and reshuffle
  addHistory(_turn, "swap", 0);
  _passStreak++;        // an exchange scores nothing, so it counts toward the stop
  _turn = (_turn + 1) % _nplayers;
  return true;
}

// End of game
// Ends when someone plays out with an empty bag, or after six scoreless turns
// (the standard "no progress" stop).
bool Game::isOver() const {
  if (_passStreak >= SCORELESS_STOP) return true;
  if (_bagN > 0) return false;
  for (uint8_t p = 0; p < _nplayers; p++) {
    bool empty = true;
    for (uint8_t i = 0; i < RACK_N; i++) if (_players[p].rack[i] != TILE_EMPTY) empty = false;
    if (empty) return true;
  }
  return false;
}

// Each player loses the value of the tiles left on their rack; if someone went
// out, they gain the sum of everyone else's leftovers.
void Game::applyFinalScores() {
  int16_t left[MAX_PLAYERS] = {0};
  int8_t  wentOut = -1;
  for (uint8_t p = 0; p < _nplayers; p++) {
    int16_t s = 0;
    bool empty = true;
    for (uint8_t i = 0; i < RACK_N; i++) {
      uint8_t t = _players[p].rack[i];
      if (t == TILE_EMPTY) continue;
      empty = false;
      s += (t == TILE_BLANK) ? 0 : letterValue(t);
    }
    left[p] = s;
    if (empty) wentOut = p;
  }
  int16_t pot = 0;
  for (uint8_t p = 0; p < _nplayers; p++) { _players[p].score -= left[p]; pot += left[p]; }
  if (wentOut >= 0) _players[wentOut].score += pot;
}

uint8_t Game::leader() const {
  uint8_t best = 0;
  for (uint8_t p = 1; p < _nplayers; p++) if (_players[p].score > _players[best].score) best = p;
  return best;
}

// Persistence — a flat binary snapshot; small enough that versioning it is
// cheaper than parsing anything structured.
#define SAVE_MAGIC 0x53435231UL   // "SCR1"

bool Game::save(const char *path) const {
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  uint32_t magic = SAVE_MAGIC;
  f.write((const uint8_t *)&magic, 4);
  f.write(&_lang, 1);
  f.write(&_nplayers, 1);
  f.write(&_turn, 1);
  f.write(&_passStreak, 1);
  uint8_t any = _anyPlayed ? 1 : 0;
  f.write(&any, 1);
  f.write((const uint8_t *)_board, sizeof(_board));
  for (uint8_t r = 0; r < BOARD_N; r++)
    for (uint8_t c = 0; c < BOARD_N; c++) { uint8_t b = _blank[r][c] ? 1 : 0; f.write(&b, 1); }
  f.write(&_bagN, 1);
  f.write(_bag, MAX_BAG);
  for (uint8_t p = 0; p < MAX_PLAYERS; p++) {
    f.write((const uint8_t *)_players[p].name, sizeof(_players[p].name));
    f.write(_players[p].rack, RACK_N);
    f.write((const uint8_t *)&_players[p].score, 2);
    uint8_t cpu = _players[p].isCpu ? 1 : 0;
    f.write(&cpu, 1);
    f.write(&_players[p].cpuLevel, 1);
  }
  f.write(&_lastN, 1);
  f.write(_lastR, RACK_N);
  f.write(_lastC, RACK_N);
  // Appended: saves written before the history existed simply end here.
  f.write(&_histN, 1);
  if (_histN) f.write((const uint8_t *)_hist, (size_t)_histN * sizeof(MoveRec));
  f.close();
  return true;
}

bool Game::load(const char *path, const Dawg *dict) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t magic = 0;
  if (f.read((uint8_t *)&magic, 4) != 4 || magic != SAVE_MAGIC) { f.close(); return false; }
  _dict = dict;
  f.read(&_lang, 1);
  f.read(&_nplayers, 1);
  f.read(&_turn, 1);
  f.read(&_passStreak, 1);
  uint8_t any = 0; f.read(&any, 1); _anyPlayed = any;
  f.read((uint8_t *)_board, sizeof(_board));
  for (uint8_t r = 0; r < BOARD_N; r++)
    for (uint8_t c = 0; c < BOARD_N; c++) { uint8_t b = 0; f.read(&b, 1); _blank[r][c] = b; }
  f.read(&_bagN, 1);
  f.read(_bag, MAX_BAG);
  for (uint8_t p = 0; p < MAX_PLAYERS; p++) {
    f.read((uint8_t *)_players[p].name, sizeof(_players[p].name));
    f.read(_players[p].rack, RACK_N);
    f.read((uint8_t *)&_players[p].score, 2);
    uint8_t cpu = 0; f.read(&cpu, 1); _players[p].isCpu = cpu;
    f.read(&_players[p].cpuLevel, 1);
  }
  f.read(&_lastN, 1);
  f.read(_lastR, RACK_N);
  f.read(_lastC, RACK_N);
  _histN = 0;
  if (f.available() >= 1) {
    int h = f.read();
    if (h > 0 && h <= MAX_HISTORY &&
        f.available() >= (int)((size_t)h * sizeof(MoveRec))) {
      f.read((uint8_t *)_hist, (size_t)h * sizeof(MoveRec));
      _histN = (uint8_t)h;
    }
  }
  f.close();
  _pendN = 0;
  if (_nplayers < 2 || _nplayers > MAX_PLAYERS) return false;
  return true;
}
