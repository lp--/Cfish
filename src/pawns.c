/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>

#include "bitboard.h"
#include "pawns.h"
#include "position.h"
#include "thread.h"

#define V(v) ((Value)(v))
#define S(mg, eg) make_score(mg, eg)

// Isolated pawn penalty by opposed flag
static const Score Isolated[2] = { S(45, 40), S(30, 27) };

// Backward pawn penalty by opposed flag
static const Score Backward[2] = { S(56, 33), S(41, 19) };

// Unsupported pawn penalty for pawns which are neither isolated nor backward.
static const Score Unsupported = S(17, 8);

// Connected pawn bonus by opposed, phalanx, twice supported and rank
static Score Connected[2][2][2][8];

// Doubled pawn penalty
static const Score Doubled = S(18,38);

// Lever bonus by rank
static const Score Lever[8] = {
  S( 0,  0), S( 0,  0), S(0, 0), S(0, 0),
  S(17, 16), S(33, 32), S(0, 0), S(0, 0)
};

  // Weakness of our pawn shelter in front of the king by [distance from edge][rank]
static const Value ShelterWeakness[][8] = {
  { V( 97), V(21), V(26), V(51), V(87), V( 89), V( 99) },
  { V(120), V( 0), V(28), V(76), V(88), V(103), V(104) },
  { V(101), V( 7), V(54), V(78), V(77), V( 92), V(101) },
  { V( 80), V(11), V(44), V(68), V(87), V( 90), V(119) }
};

  // Danger of enemy pawns moving toward our king by [type][distance from edge][rank]
static const Value StormDanger[][4][8] = {
  { { V( 0),  V(  67), V( 134), V(38), V(32) },
    { V( 0),  V(  57), V( 139), V(37), V(22) },
    { V( 0),  V(  43), V( 115), V(43), V(27) },
    { V( 0),  V(  68), V( 124), V(57), V(32) } },
  { { V(20),  V(  43), V( 100), V(56), V(20) },
    { V(23),  V(  20), V(  98), V(40), V(15) },
    { V(23),  V(  39), V( 103), V(36), V(18) },
    { V(28),  V(  19), V( 108), V(42), V(26) } },
  { { V( 0),  V(   0), V(  75), V(14), V( 2) },
    { V( 0),  V(   0), V( 150), V(30), V( 4) },
    { V( 0),  V(   0), V( 160), V(22), V( 5) },
    { V( 0),  V(   0), V( 166), V(24), V(13) } },
  { { V( 0),  V(-283), V(-281), V(57), V(31) },
    { V( 0),  V(  58), V( 141), V(39), V(18) },
    { V( 0),  V(  65), V( 142), V(48), V(32) },
    { V( 0),  V(  60), V( 126), V(51), V(19) } }
};

// Max bonus for king safety. Corresponds to start position with all the pawns
// in front of the king and no enemy pawn on the horizon.
static const Value MaxSafetyBonus = V(258);

#undef S
#undef V

INLINE Score pawn_evaluate(const Pos *pos, PawnEntry *e, const int Us)
{
  const int Up    = (Us == WHITE ? DELTA_N  : DELTA_S);
  const int Right = (Us == WHITE ? DELTA_NE : DELTA_SW);
  const int Left  = (Us == WHITE ? DELTA_NW : DELTA_SE);

  Bitboard b, neighbours, stoppers, doubled, supported, phalanx;
  Square s;
  int opposed, lever, connected, backward;
  Score score = SCORE_ZERO;
  const Bitboard* pawnAttacksBB = StepAttacksBB[make_piece(Us, PAWN)];

  Bitboard ourPawns   = pieces_cp(Us, PAWN);
  Bitboard theirPawns = pieces_p(PAWN) ^ ourPawns;

  e->passedPawns[Us] = e->pawnAttacksSpan[Us] = 0;
  e->kingSquares[Us] = SQ_NONE;
  e->semiopenFiles[Us] = 0xFF;
  e->pawnAttacks[Us] = shift_bb(Right, ourPawns) | shift_bb(Left, ourPawns);
  e->pawnsOnSquares[Us][BLACK] = popcount(ourPawns & DarkSquares);
  e->pawnsOnSquares[Us][WHITE] = popcount(ourPawns & LightSquares);

  // Loop through all pawns of the current color and score each pawn
  loop_through_pieces(Us, PAWN, s) {
    assert(piece_on(s) == make_piece(Us, PAWN));

    int f = file_of(s);

    e->semiopenFiles[Us] &= ~(1 << f);
    e->pawnAttacksSpan[Us] |= pawn_attack_span(Us, s);

    // Flag the pawn
    opposed    = !!(theirPawns & forward_bb(Us, s));
    stoppers   = theirPawns & passed_pawn_mask(Us, s);
    lever      = !!(theirPawns & pawnAttacksBB[s]);
    doubled    = ourPawns   & sq_bb(s + Up);
    neighbours = ourPawns   & adjacent_files_bb(f);
    phalanx    = neighbours & rank_bb_s(s);
    supported  = neighbours & rank_bb_s(s - Up);
    connected  = !!(supported | phalanx);

    // A pawn is backward when it is behind all pawns of the same color on the
    // adjacent files and cannot be safely advanced.
    if (!neighbours || lever || relative_rank_s(Us, s) >= RANK_5)
      backward = 0;
    else {
      // Find the backmost rank with neighbours or stoppers
      b = rank_bb_s(backmost_sq(Us, neighbours | stoppers));

      // The pawn is backward when it cannot safely progress to that rank:
      // either there is a stopper in the way on this rank, or there is a
      // stopper on adjacent file which controls the way to that rank.
      backward = !!((b | shift_bb(Up, b & adjacent_files_bb(f))) & stoppers);

      assert(!backward || !(pawn_attack_span(Us ^ 1, s + Up) & neighbours));
    }

    // Passed pawns will be properly scored in evaluation because we need
    // full attack info to evaluate them.
    if (!stoppers && !(ourPawns & forward_bb(Us, s)))
      e->passedPawns[Us] |= sq_bb(s);

    // Score this pawn
    if (!neighbours)
      score -= Isolated[opposed];

    else if (backward)
      score -= Backward[opposed];

    else if (!supported)
      score -= Unsupported;

    if (connected)
      score += Connected[opposed][!!phalanx][!!more_than_one(supported)][relative_rank_s(Us, s)];

    if (doubled)
      score -= Doubled;

    if (lever)
      score += Lever[relative_rank_s(Us, s)];
  }

  return score;
}


// pawn_init() initializes some tables needed by evaluation.

void pawn_init(void)
{
  static const int Seed[8] = { 0, 8, 19, 13, 71, 94, 169, 324 };

  for (int opposed = 0; opposed < 2; opposed++)
    for (int phalanx = 0; phalanx < 2; phalanx++)
      for (int apex = 0; apex < 2; apex++)
        for (int r = RANK_2; r < RANK_8; ++r) {
          int v = (Seed[r] + (phalanx ? (Seed[r + 1] - Seed[r]) / 2 : 0)) >> opposed;
          v += (apex ? v / 2 : 0);
          Connected[opposed][phalanx][apex][r] = make_score(v, v * 5 / 8);
      }
}


// pawns_probe() looks up the current position's pawns configuration in
// the pawns hash table.

void pawn_entry_fill(const Pos *pos, PawnEntry *e, Key key)
{
  e->key = key;
  e->score = pawn_evaluate(pos, e, WHITE) - pawn_evaluate(pos, e, BLACK);
  e->asymmetry = popcount(e->semiopenFiles[WHITE] ^ e->semiopenFiles[BLACK]);
  e->openFiles = popcount(e->semiopenFiles[WHITE] & e->semiopenFiles[BLACK]);
}


// shelter_storm() calculates shelter and storm penalties for the file
// the king is on, as well as the two adjacent files.

INLINE Value shelter_storm(const Pos *pos, Square ksq, const int Us)
{
  const int Them = (Us == WHITE ? BLACK : WHITE);
  
  enum { NoFriendlyPawn, Unblocked, BlockedByPawn, BlockedByKing };

  Bitboard b = pieces_p(PAWN) & (in_front_bb(Us, rank_of(ksq)) | rank_bb_s(ksq));
  Bitboard ourPawns = b & pieces_c(Us);
  Bitboard theirPawns = b & pieces_c(Them);
  Value safety = MaxSafetyBonus;
  uint32_t center = max(FILE_B, min(FILE_G, file_of(ksq)));

  for (uint32_t f = center - 1; f <= center + 1; f++) {
    b = ourPawns & file_bb(f);
    uint32_t rkUs = b ? relative_rank_s(Us, backmost_sq(Us, b)) : RANK_1;

    b  = theirPawns & file_bb(f);
    uint32_t rkThem = b ? relative_rank_s(Us, frontmost_sq(Them, b)) : RANK_1;

    safety -=  ShelterWeakness[min(f, FILE_H - f)][rkUs]
             + StormDanger
               [f == file_of(ksq) && rkThem == relative_rank_s(Us, ksq) + 1 ? BlockedByKing  :
                rkUs   == RANK_1                                            ? NoFriendlyPawn :
                rkThem == rkUs + 1                                          ? BlockedByPawn  : Unblocked]
               [min(f, FILE_H - f)][rkThem];
  }

  return safety;
}


// do_king_safety() calculates a bonus for king safety. It is called only
// when king square changes, which is about 20% of total king_safety() calls.

INLINE Score do_king_safety(PawnEntry *pe, const Pos *pos, Square ksq,
                                   const int Us)
{
  pe->kingSquares[Us] = ksq;
  pe->castlingRights[Us] = can_castle_c(Us);
  int minKingPawnDistance = 0;

  Bitboard pawns = pieces_cp(Us, PAWN);
  if (pawns)
    while (!(DistanceRingBB[ksq][minKingPawnDistance++] & pawns)) {}

  Value bonus = shelter_storm(pos, ksq, Us);

  // If we can castle use the bonus after the castling if it is bigger
  if (can_castle_cr(make_castling_right(Us, KING_SIDE)))
    bonus = max(bonus, shelter_storm(pos, relative_square(Us, SQ_G1), Us));

  if (can_castle_cr(make_castling_right(Us, QUEEN_SIDE)))
    bonus = max(bonus, shelter_storm(pos, relative_square(Us, SQ_C1), Us));

  return make_score(bonus, -16 * minKingPawnDistance);
}

// "template" instantiation:
Score do_king_safety_white(PawnEntry *pe, const Pos *pos, Square ksq)
{
  return do_king_safety(pe, pos, ksq, WHITE);
}

Score do_king_safety_black(PawnEntry *pe, const Pos *pos, Square ksq)
{
  return do_king_safety(pe, pos, ksq, BLACK);
}

