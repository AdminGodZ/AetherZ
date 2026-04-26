#pragma once

#include "engine/board.hpp"

namespace aetherz {

[[nodiscard]] Bitboard pawn_attacks(Color color, int square);
[[nodiscard]] Bitboard knight_attacks(int square);
[[nodiscard]] Bitboard bishop_attacks(int square, Bitboard occupancy);
[[nodiscard]] Bitboard rook_attacks(int square, Bitboard occupancy);
[[nodiscard]] Bitboard queen_attacks(int square, Bitboard occupancy);
[[nodiscard]] Bitboard king_attacks(int square);
[[nodiscard]] Bitboard attacks_from(PieceType type, Color color, int square, Bitboard occupancy);
[[nodiscard]] Bitboard attackers_to(const Board& board, int square, Color color, Bitboard occupancy);

} // namespace aetherz
