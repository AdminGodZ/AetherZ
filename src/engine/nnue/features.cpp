#include "engine/nnue/features.hpp"

namespace aetherz::nnue {

std::vector<int> active_halfka_features(const Board& board, Color perspective)
{
    std::vector<int> features;
    features.reserve(32);

    const int king = board.king_square(perspective);
    if (!is_valid_square(king)) {
        return features;
    }

    const auto& squares = board.squares();
    for (int square = 0; square < 64; ++square) {
        const Piece piece = squares[square];
        if (piece == Piece::None) {
            continue;
        }

        const int feature = halfka_feature_index(perspective, king, piece, square);
        if (feature >= 0) {
            features.push_back(feature);
        }
    }

    return features;
}

DirtyMove dirty_move_from(const Board& board, const Board::UndoState& undo)
{
    DirtyMove dirty;
    const auto& after = board.squares();

    for (int square = 0; square < 64; ++square) {
        const Piece before_piece = undo.squares[square];
        const Piece after_piece = after[square];
        if (before_piece == after_piece) {
            continue;
        }

        if (before_piece != Piece::None && dirty.removed_count < static_cast<int>(dirty.removed.size())) {
            dirty.removed[dirty.removed_count++] = PieceSquare{.piece = before_piece, .square = square};
        }
        if (after_piece != Piece::None && dirty.added_count < static_cast<int>(dirty.added.size())) {
            dirty.added[dirty.added_count++] = PieceSquare{.piece = after_piece, .square = square};
        }
    }

    return dirty;
}

} // namespace aetherz::nnue
