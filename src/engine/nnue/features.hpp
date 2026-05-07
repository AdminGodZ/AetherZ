#pragma once

#include "engine/board.hpp"

#include <array>
#include <vector>

namespace aetherz::nnue {

struct PieceSquare {
    Piece piece = Piece::None;
    int square = -1;
};

struct DirtyMove {
    std::array<PieceSquare, 4> removed{};
    std::array<PieceSquare, 4> added{};
    int removed_count = 0;
    int added_count = 0;
};

inline constexpr int halfka_king_buckets = 64;
inline constexpr int halfka_piece_kinds = 12;
inline constexpr int halfka_squares = 64;
inline constexpr int halfka_perspective_feature_count = halfka_king_buckets * halfka_piece_kinds * halfka_squares;

[[nodiscard]] constexpr int orient_square(Color perspective, int square)
{
    return perspective == Color::White ? square : square ^ 56;
}

[[nodiscard]] inline int halfka_feature_index(Color perspective, int king_square, Piece piece, int piece_square)
{
    if (!is_valid_square(king_square) || !is_valid_square(piece_square) || piece == Piece::None) {
        return -1;
    }

    const int relative_king = orient_square(perspective, king_square);
    const int relative_piece_square = orient_square(perspective, piece_square);
    return (relative_king * halfka_piece_kinds + piece_index(piece)) * halfka_squares + relative_piece_square;
}

[[nodiscard]] std::vector<int> active_halfka_features(const aetherz::Board& board, Color perspective);
[[nodiscard]] DirtyMove dirty_move_from(const aetherz::Board& board, const aetherz::Board::UndoState& undo);

} // namespace aetherz::nnue
