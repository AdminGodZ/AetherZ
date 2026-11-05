#include "engine/see.hpp"

#include "engine/attacks.hpp"
#include "engine/evaluate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

namespace aetherz {
namespace {

struct SelectedAttacker {
    int square = -1;
    PieceType type = PieceType::None;
};

constexpr std::array<PieceType, 6> attacker_order{
    PieceType::Pawn,
    PieceType::Knight,
    PieceType::Bishop,
    PieceType::Rook,
    PieceType::Queen,
    PieceType::King,
};

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

int captured_square(const Board& board, const Move& move)
{
    if (move.flag != MoveFlag::EnPassant) {
        return move.to;
    }

    return board.side_to_move() == Color::White ? move.to - 8 : move.to + 8;
}

std::optional<SelectedAttacker> least_valuable_attacker(
    const Board& board,
    Color color,
    int target_square,
    Bitboard attackers,
    Bitboard occupancy)
{
    for (const PieceType type : attacker_order) {
        Bitboard candidates = attackers & board.pieces(color, type) & occupancy;
        while (candidates != 0) {
            const int square = std::countr_zero(candidates);
            candidates &= candidates - 1;

            if (type == PieceType::King
                && attackers_to(board, target_square, opposite(color), occupancy & ~square_bb(square)) != 0) {
                continue;
            }

            return SelectedAttacker{
                .square = square,
                .type = type,
            };
        }
    }

    return std::nullopt;
}

// Whether the move would leave the mover's own king attacked.
bool leaves_king_in_check(const Board& board, const Move& move)
{
    const Color us = board.side_to_move();
    const int captured = captured_square(board, move);
    Bitboard occupancy = (board.occupancy() & ~square_bb(move.from) & ~square_bb(captured)) | square_bb(move.to);
    const int king = piece_type(board.piece_at(move.from)) == PieceType::King ? move.to : board.king_square(us);
    if (!is_valid_square(king)) {
        return false;
    }

    // The captured piece no longer attacks anything.
    const Bitboard attackers = attackers_to(board, king, opposite(us), occupancy) & ~square_bb(captured);
    return attackers != 0;
}

} // namespace

int static_exchange_evaluation(const Board& board, const Move& move)
{
    if (!move.is_valid() || (!is_capture_like(move) && move.promotion == PieceType::None)) {
        return 0;
    }

    const Piece moving_piece = board.piece_at(move.from);
    if (moving_piece == Piece::None || piece_color(moving_piece) != board.side_to_move()) {
        return 0;
    }

    if (leaves_king_in_check(board, move)) {
        return 0;
    }

    std::array<int, 32> gains{};
    gains[0] = captured_value(board, move) + promotion_delta(move);

    Bitboard occupancy = board.occupancy();
    occupancy &= ~square_bb(move.from);
    if (move.flag == MoveFlag::EnPassant) {
        occupancy &= ~square_bb(captured_square(board, move));
    }
    occupancy |= square_bb(move.to);

    int target_value = piece_value(
        move.promotion == PieceType::None ? piece_type(moving_piece) : move.promotion);
    Color side = opposite(board.side_to_move());
    int depth = 0;

    while (depth + 1 < static_cast<int>(gains.size())) {
        const Bitboard attackers = attackers_to(board, move.to, side, occupancy);
        const auto attacker = least_valuable_attacker(board, side, move.to, attackers, occupancy);
        if (!attacker) {
            break;
        }

        ++depth;
        gains[depth] = target_value - gains[depth - 1];

        occupancy &= ~square_bb(attacker->square);
        target_value = piece_value(attacker->type);
        side = opposite(side);
    }

    while (depth > 0) {
        --depth;
        gains[depth] = -std::max(-gains[depth], gains[depth + 1]);
    }

    return gains[0];
}

} // namespace aetherz
