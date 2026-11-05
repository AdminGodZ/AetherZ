#pragma once

#include "engine/board.hpp"

#include <array>
#include <vector>

namespace aetherz {

struct EvalScore {
    int middlegame = 0;
    int endgame = 0;
};

// Positions of the evaluation weights in the flat weight table. Indices
// followed by a count cover one weight per piece type, square or rank.
namespace eval_index {

inline constexpr int material = 0;                                  // 6 piece types
inline constexpr int piece_square = material + 6;                   // 6 x 64, own point of view
inline constexpr int passed_pawn = piece_square + 6 * 64;           // 8 relative ranks
inline constexpr int mobility = passed_pawn + 8;                    // 6 piece types
inline constexpr int doubled_pawn = mobility + 6;
inline constexpr int isolated_pawn = doubled_pawn + 1;
inline constexpr int bishop_pair = isolated_pawn + 1;
inline constexpr int rook_open_file = bishop_pair + 1;
inline constexpr int rook_semi_open_file = rook_open_file + 1;
inline constexpr int rook_seventh_rank = rook_semi_open_file + 1;
inline constexpr int threat_hanging = rook_seventh_rank + 1;        // 6 victim types
inline constexpr int threat_by_pawn = threat_hanging + 6;           // 6 victim types
inline constexpr int outpost_knight_central = threat_by_pawn + 6;
inline constexpr int outpost_knight = outpost_knight_central + 1;
inline constexpr int outpost_bishop_central = outpost_knight + 1;
inline constexpr int outpost_bishop = outpost_bishop_central + 1;
inline constexpr int king_closeness = outpost_bishop + 1;           // 6 piece types
inline constexpr int king_zone_presence = king_closeness + 6;       // 6 piece types
inline constexpr int king_zone_attack = king_zone_presence + 6;     // 6 piece types
inline constexpr int king_open_file = king_zone_attack + 6;
inline constexpr int king_adjacent_open_file = king_open_file + 1;
inline constexpr int pawn_shield = king_adjacent_open_file + 1;
inline constexpr int pawn_shield_missing = pawn_shield + 1;
inline constexpr int castled = pawn_shield_missing + 1;
inline constexpr int uncastled_with_rights = castled + 1;
inline constexpr int uncastled_without_rights = uncastled_with_rights + 1;
inline constexpr int king_walk = uncastled_without_rights + 1;
inline constexpr int king_central_file = king_walk + 1;
inline constexpr int count = king_central_file + 1;

} // namespace eval_index

using EvalWeights = std::array<EvalScore, eval_index::count>;

[[nodiscard]] const EvalWeights& eval_weights();

// The evaluation is linear in its weights: for a position it is
//   (phase * sum(c_i * mg_i) + (24 - phase) * sum(c_i * eg_i)) / 24
// where c_i counts how often weight i applies for white minus for black.
struct EvalTrace {
    std::array<int, eval_index::count> coefficients{};
    int phase = 0;
};

inline constexpr int max_eval_phase = 24;

// Static exchange values; fixed so that tuning the evaluation does not
// change move ordering and pruning decisions.
[[nodiscard]] int middlegame_piece_value(PieceType type);
[[nodiscard]] int endgame_piece_value(PieceType type);
[[nodiscard]] int evaluate_white(const Board& board);
[[nodiscard]] int evaluate_side_to_move(const Board& board);
[[nodiscard]] int evaluate_white_traced(const Board& board, EvalTrace& trace);

} // namespace aetherz
