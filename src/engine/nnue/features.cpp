#include "engine/nnue/features.hpp"

namespace aetherz::nnue {

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
    // The piece now on the target square is the promoted piece for promotions.
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
