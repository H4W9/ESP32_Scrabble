// cpu.h
// Move generation for the computer opponent.
//
// This is the Appel & Jacobson algorithm over the plain forward DAWG we already
// load — no GADDAG. The classic three pieces:
//
//   anchors      empty squares adjacent to a tile already on the board (on an
//                empty board, just the centre star)
//   cross-checks for each empty square, the set of letters that would still form
//                a valid word on the PERPENDICULAR axis; precomputed per line so
//                the inner search never has to look sideways
//   left-part /  build every legal prefix reaching an anchor (either the fixed
//   extend-right tiles already on the board, or letters from the rack), then walk
//                the DAWG rightward laying tiles until a word terminates
//
// Run once for horizontal play and once for vertical, which is why every board
// access goes through mapSq() rather than touching (row, col) directly.
//
// SELECTION is a separate stage: a generator proposes, a chooser decides.
//
//   generate     everything legal, shortlisted by raw score per orientation
//   answerPower  what each move is actually worth to play — score plus board
//                development, minus what it leaves you holding
//   cpuPick      the opponent's taste: a difficulty-width range down the power
//                ranking, occasionally re-ranked by one trait instead
//
// Master shows the same list ordered purely by power, so its suggestions and the
// opponent's play come from one engine.
//
// Scoring deliberately reuses Game::scorePending() instead of reimplementing it:
// the generator places its candidate as pending tiles, asks the engine what it
// is worth, and takes it back. That guarantees the CPU and the human are scored
// by exactly the same code.

#pragma once
#include <Arduino.h>
#include "game.h"

struct CpuPlacement {
  uint8_t row, col, rackIdx, letter;
  bool    isBlank;
};

struct CpuMove {
  CpuPlacement p[RACK_N];
  uint8_t      n = 0;          // tiles laid
  uint8_t      crossings = 0;  // existing tiles reused + cross-words formed
  bool         horiz = false;  // orientation, for diagnostics
  int          score = 0;

  // Everything below feeds answerPower(). Gathered while the candidate is still
  // placed, because none of it can be recovered from the placements alone.
  uint8_t leave[RACK_N];       // rack tiles this move does NOT use
  uint8_t nleave = 0;
  uint8_t onEmpty = 0;         // tiles laid where nothing was adjacent (see .cpp)
  uint8_t wordLen = 0;         // length of the main word, board tiles included
  bool    sOnEnd = false;      // spends an S just to pluralise
  int     power = 0;           // the evaluation this move is chosen by
};

// Optional abort hook, polled during the search. Return true to give up early:
// the caller then gets whatever was found so far (possibly nothing). Without it
// a search runs for its whole budget with the UI frozen, which makes a slow turn
// look like a hang and leaves no way out.
typedef bool (*CpuAbortFn)();
void cpuSetAbortHook(CpuAbortFn fn);
// True if the last search stopped because the hook asked it to.
bool cpuWasAborted();

// Search for a move for the current player.
//   level 0 = easy, 1 = normal, 2 = hard (see cpuPick in the .cpp)
//   budgetMs caps the search so a turn can never hang the UI; the best move
//   found so far is used when the budget runs out.
// Returns false if the player has no legal move at all (caller should pass).
// `fallback` is searched only if `dict` produced nothing at all. The opponent
// normally plays from the reduced everyday list, and on a developed board that
// list plus the one-cross-word rule can genuinely leave it with no move — which
// came out as an opponent that skipped far too often. Widening the VOCABULARY
// is the right retry; the cross-word rule is a house rule about board shape and
// is never relaxed.
bool cpuFindMove(Game &g, const Dawg &dict, uint8_t level, CpuMove &out,
                 uint32_t budgetMs = 4000, const Dawg *fallback = nullptr);

// The ranked list itself, best first — what the Master screen shows. Returns how
// many were written (up to maxN).
uint8_t cpuFindMoves(Game &g, const Dawg &dict, CpuMove *out, uint8_t maxN,
                     uint32_t budgetMs = 4000, const Dawg *fallback = nullptr);

// Apply a found move to the game as pending tiles, ready for Game::commit().
void cpuApply(Game &g, const CpuMove &m);
