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
    if (undo.null_move) {
        return dirty;
    }

    auto remove = [&](Piece piece, int square) {
        dirty.removed[dirty.removed_count++] = PieceSquare{.piece = piece, .square = square};
    };
    auto add = [&](Piece piece, int square) {
        dirty.added[dirty.added_count++] = PieceSquare{.piece = piece, .square = square};
    };

    const Move& move = undo.move;
    remove(undo.moved, move.from);
    if (undo.captured != Piece::None) {
        remove(undo.captured, undo.captured_square);
    }
    add(board.piece_at(move.to), move.to);

    if (move.flag == MoveFlag::KingCastle || move.flag == MoveFlag::QueenCastle) {
        const bool white = piece_color(undo.moved) == Color::White;
        const bool kingside = move.flag == MoveFlag::KingCastle;
        const int rook_from = white ? (kingside ? 7 : 0) : (kingside ? 63 : 56);
        const int rook_to = white ? (kingside ? 5 : 3) : (kingside ? 61 : 59);
        const Piece rook = make_piece(white ? Color::White : Color::Black, PieceType::Rook);
        remove(rook, rook_from);
        add(rook, rook_to);
    }

    return dirty;
}

} // namespace aetherz::nnue
