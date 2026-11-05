#pragma once

#include "engine/board.hpp"

#include <array>

namespace aetherz::nnue {

struct PieceSquare {
    Piece piece = Piece::None;
    int square = -1;
};

// The pieces a move takes off and puts on the board. Null moves change
// nothing.
struct DirtyMove {
    std::array<PieceSquare, 2> removed{};
    std::array<PieceSquare, 2> added{};
    int removed_count = 0;
    int added_count = 0;
};

// Bullet's Chess768 input: own pieces first, then the opponent's, each by
// piece type and square, with the board flipped vertically for Black.
[[nodiscard]] constexpr int feature_index(Color perspective, Piece piece, int square)
{
    const int side = piece_color(piece) == perspective ? 0 : 384;
    const int relative_square = perspective == Color::White ? square : square ^ 56;
    return side + piece_type_index(piece_type(piece)) * 64 + relative_square;
}

[[nodiscard]] DirtyMove dirty_move_from(const Board& board, const Board::UndoState& undo);

} // namespace aetherz::nnue
