#include "engine/see.hpp"

#include "engine/evaluate.hpp"

#include <algorithm>

namespace aetherz {
namespace {

bool is_capture_like(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::PromotionCapture;
}

int piece_value(PieceType type)
{
    return middlegame_piece_value(type);
}

int promotion_delta(const Move& move)
{
    if (move.promotion == PieceType::None) {
        return 0;
    }

    return piece_value(move.promotion) - piece_value(PieceType::Pawn);
}

int captured_value(const Board& board, const Move& move)
{
    if (!is_capture_like(move)) {
        return 0;
    }

    if (move.flag == MoveFlag::EnPassant) {
        return piece_value(PieceType::Pawn);
    }

    return piece_value(piece_type(board.piece_at(move.to)));
}

int material_gain(const Board& board, const Move& move)
{
    return captured_value(board, move) + promotion_delta(move);
}

bool recaptures_square(const Move& move, int square)
{
    return move.to == square && is_capture_like(move);
}

int best_exchange_reply(Board& board, int target_square)
{
    int best = 0;

    for (const Move& move : board.generate_legal_moves()) {
        if (!recaptures_square(move, target_square)) {
            continue;
        }

        const int gain = material_gain(board, move);
        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }

        const int score = gain - best_exchange_reply(board, target_square);
        board.unmake_move(undo);
        best = std::max(best, score);
    }

    return best;
}

} // namespace

int static_exchange_evaluation(const Board& board, const Move& move)
{
    if (!move.is_valid() || (!is_capture_like(move) && move.promotion == PieceType::None)) {
        return 0;
    }

    Board next = board;
    Board::UndoState undo;
    if (!next.make_move(move, undo) || next.in_check(board.side_to_move())) {
        return 0;
    }

    return material_gain(board, move) - best_exchange_reply(next, move.to);
}

} // namespace aetherz
