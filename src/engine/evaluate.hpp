#pragma once

#include "engine/board.hpp"

namespace aetherz {

[[nodiscard]] int middlegame_piece_value(PieceType type);
[[nodiscard]] int endgame_piece_value(PieceType type);
[[nodiscard]] int evaluate_white(const Board& board);
[[nodiscard]] int evaluate_side_to_move(const Board& board);

} // namespace aetherz
