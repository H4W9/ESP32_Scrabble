// game.h
// Scrabble rules engine: board state, tile bag, racks, move validation and
// scoring. Deliberately free of any drawing or touch code so it can be reasoned
// about (and later unit-tested off-device) on its own.
//
// Letters are the firmware's letter bytes throughout: 'A'..'Z' plus 0x80 (Ae),
// 0x82 (Oe), 0x84 (Ue). 0 means "empty square". A blank tile is carried as
// TILE_BLANK in a rack, but once played it occupies the board as the letter it
// was declared to be, with its blank-ness recorded separately so it scores 0.
//
// Note there is NO eszett tile: German Scrabble is 102 tiles with no ss, and a
// word containing one is played SS. generate_scrabble_dict.py folds it the same
// way, so the word list and the bag agree.

#pragma once
#include <Arduino.h>
#include "board_theme.h"
#include "dawg.h"
#include "letters.h"

#define BOARD_N        15
#define RACK_N          7
#define MAX_PLAYERS     4
#define MAX_BAG       104
#define TILE_BLANK      1     // rack-only marker; never stored on the board
#define TILE_EMPTY      0

// The bonus for laying all seven rack tiles is per-language editable config —
// see DIST_DEFAULT_BINGO in letters.h and Settings -> Letters.

// The game also ends after this many consecutive scoreless turns. Six is the
// standard "no progress" stop, and it is a flat count of turns, NOT a count
// scaled by player number -- with four players a scaled rule would drag on for
// three full rounds of nothing.
#define SCORELESS_STOP  6

// How many PERPENDICULAR words one move may create, on top of the word it is
// actually playing. One keeps the board reading like a crossword: MEDIC forms
// none (it reuses an existing E), VIAND forms one (DA), VINCA forms one (AA).
// Standard Scrabble permits any number so long as all are valid, so this is
// deliberately stricter. Raise it to 7 to get the usual rule.
#define MAX_CROSS_WORDS 1

// A committed turn, for the Words-played history. Kept as one chronological
// list rather than per-player arrays so the capacity is shared -- a game where
// one side passes a lot would otherwise waste half of it.
#define MAX_HISTORY 64
struct MoveRec {
  uint8_t player;
  int16_t score;
  char    word[14];        // "" for a pass, "swap" for an exchange
};

// One tile placed this turn but not yet committed.
struct Pending {
  uint8_t row, col;
  uint8_t letter;      // the letter it counts as (a declared blank included)
  uint8_t rackIdx;     // which rack slot it came from, so it can be taken back
  bool    isBlank;     // played from a blank: scores 0
};

struct Player {
  char    name[12];
  uint8_t rack[RACK_N];   // TILE_EMPTY for an empty slot
  int16_t score;
  bool    isCpu;
  uint8_t cpuLevel;       // 0 = easy, 1 = normal, 2 = hard (used at M3)
};

// Why a move was rejected, so the UI can say something specific.
enum MoveErr : uint8_t {
  MV_OK = 0,
  MV_NO_TILES,        // nothing placed
  MV_NOT_IN_LINE,     // placed tiles not all in one row/column
  MV_HAS_GAP,         // gap between placed tiles with no board tile filling it
  MV_NOT_CENTRE,      // opening move must cover the centre star
  MV_NOT_CONNECTED,   // later moves must touch something already on the board
  MV_NO_WORD,         // placement forms nothing at least two letters long
  MV_TOO_MANY_CROSS,  // more perpendicular words than MAX_CROSS_WORDS
  MV_BAD_WORD,        // a formed word is not in the word list
};

class Game {
public:
  // Setup
  void begin(const Dawg *dict, uint8_t lang, uint8_t numPlayers);
  void setPlayer(uint8_t i, const char *name, bool isCpu, uint8_t cpuLevel = 1);

  // Board
  uint8_t at(uint8_t r, uint8_t c) const { return _board[r][c]; }
  bool    isBlankAt(uint8_t r, uint8_t c) const { return _blank[r][c]; }
  bool    isEmpty(uint8_t r, uint8_t c) const { return _board[r][c] == TILE_EMPTY; }
  bool    firstMovePlayed() const { return _anyPlayed; }

  // Turn state
  uint8_t current() const { return _turn; }
  Player &player(uint8_t i) { return _players[i]; }
  uint8_t numPlayers() const { return _nplayers; }
  uint8_t bagCount() const { return _bagN; }

  // Placing tiles for the current player
  bool    place(uint8_t r, uint8_t c, uint8_t rackIdx, uint8_t declaredLetter);
  bool    unplaceAt(uint8_t r, uint8_t c);
  void    clearPending();
  uint8_t pendingCount() const { return _pendN; }
  const Pending *pending() const { return _pend; }
  // The letter shown at a square, counting tiles placed this turn.
  uint8_t shownAt(uint8_t r, uint8_t c) const;
  bool    isPendingAt(uint8_t r, uint8_t c) const;
  // Reorder the tray by dragging a tile to another slot. Tiles already laid out
  // this turn remember which slot they came from, so those indices are remapped
  // along with the tiles — otherwise a reorder would silently repoint them.
  void    moveRackTile(uint8_t from, uint8_t to);

  // Rules
  MoveErr validate(String *badWord = nullptr) const;
  int     scorePending(String *wordsOut = nullptr) const;
  // How interlocked a pending move is: tiles it reuses from the board plus the
  // number of cross-words it forms. The CPU prefers higher values so the board
  // grows like a crossword instead of drifting in one direction.
  uint8_t pendingCrossings() const;
  // Just the perpendicular words of 2+ letters a pending move would create.
  uint8_t pendingCrossWords() const;
  // The active cross-word limit. Normally MAX_CROSS_WORDS, but the opponent
  // raises it for one turn when the strict rule leaves it no legal move at all
  // -- skipping every turn on a crowded board is worse than one untidy play.
  uint8_t crossLimit() const { return _crossLimit; }
  void    setCrossLimit(uint8_t n) { _crossLimit = n; }

  // Luck helper: spread the bag by letter and steer each draw towards a
  // playable vowel/consonant mix, so a hand of seven consonants stops being a
  // thing that happens. See shuffleBag()/refill() for what it actually does.
  bool    luckHelper() const { return _luckHelper; }
  void    setLuckHelper(bool on) { _luckHelper = on; }

  static bool isVowel(uint8_t tile);
  bool    commit();                 // validate, score, refill rack, next player
  void    pass();
  bool    exchange(const bool *which);   // needs >= RACK_N tiles left in the bag

  // End of game
  bool    isOver() const;
  void    applyFinalScores();
  uint8_t leader() const;

  // Persistence (SD)
  bool save(const char *path) const;
  bool load(const char *path, const Dawg *dict);

  // Letter values for the active language, for rendering tiles.
  uint8_t letterValue(uint8_t letter) const;

  uint8_t lang() const { return _lang; }
  uint8_t lastMoveCount() const { return _lastN; }

  // Move history (Words played)
  uint8_t historyCount() const { return _histN; }
  const MoveRec &history(uint8_t i) const { return _hist[i]; }
  bool    wasLastMove(uint8_t r, uint8_t c) const;

private:
  const Dawg *_dict = nullptr;
  uint8_t _lang = 0;

  uint8_t _board[BOARD_N][BOARD_N];
  bool    _blank[BOARD_N][BOARD_N];
  bool    _anyPlayed = false;

  uint8_t _bag[MAX_BAG];
  uint8_t _bagN = 0;

  Player  _players[MAX_PLAYERS];
  uint8_t _nplayers = 2;
  uint8_t _turn = 0;
  uint8_t _passStreak = 0;          // six consecutive passes ends the game
  uint8_t _crossLimit = MAX_CROSS_WORDS;
  bool    _luckHelper = true;

  Pending _pend[RACK_N];
  uint8_t _pendN = 0;

  MoveRec _hist[MAX_HISTORY];
  uint8_t _histN = 0;
  void    addHistory(uint8_t p, const char *word, int score);

  // Squares filled by the previous committed move, for the "last move" tint.
  uint8_t _lastR[RACK_N], _lastC[RACK_N], _lastN = 0;

  void fillBag();
  void shuffleBag();
  bool drawTile(uint8_t &out);
  // want > 0 prefers a vowel, < 0 a consonant, 0 no preference.
  bool drawTilePreferring(uint8_t &out, int want, const uint8_t *rack);
  void refill(uint8_t p);
  // Walk from (r,c) to the start of its word along `dr,dc`, then read the word.
  uint8_t readWord(uint8_t r, uint8_t c, int8_t dr, int8_t dc,
                   uint8_t *out, uint8_t *rows, uint8_t *cols) const;
  int  scoreWord(const uint8_t *rows, const uint8_t *cols, uint8_t len) const;
  bool wordOk(const uint8_t *w, uint8_t len) const;
};
