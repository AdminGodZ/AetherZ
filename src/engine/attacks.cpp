#include "engine/attacks.hpp"

#include <array>
#include <utility>

namespace aetherz {
namespace {

constexpr std::array<std::pair<int, int>, 4> bishop_directions{{
    {1, 1},
    {-1, 1},
    {1, -1},
    {-1, -1},
}};

constexpr std::array<std::pair<int, int>, 4> rook_directions{{
    {1, 0},
    {-1, 0},
    {0, 1},
    {0, -1},
}};

constexpr std::array<std::pair<int, int>, 8> knight_offsets{{
    {1, 2},
    {2, 1},
    {2, -1},
    {1, -2},
    {-1, -2},
    {-2, -1},
    {-2, 1},
    {-1, 2},
}};

constexpr std::array<std::pair<int, int>, 8> king_offsets{{
    {1, 1},
    {1, 0},
    {1, -1},
    {0, -1},
    {-1, -1},
    {-1, 0},
    {-1, 1},
    {0, 1},
}};

Bitboard leaper_attacks(int square, const std::array<std::pair<int, int>, 8>& offsets)
{
    if (!is_valid_square(square)) {
        return 0;
    }

    Bitboard attacks = 0;
    const int file = file_of(square);
    const int rank = rank_of(square);
    for (const auto& [delta_file, delta_rank] : offsets) {
        const int target_file = file + delta_file;
        const int target_rank = rank + delta_rank;
        if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) {
            continue;
        }

        attacks |= square_bb(target_rank * 8 + target_file);
    }
    return attacks;
}

Bitboard ray_attacks(
    int square,
    Bitboard occupancy,
    const std::array<std::pair<int, int>, 4>& directions)
{
    if (!is_valid_square(square)) {
        return 0;
    }

    Bitboard attacks = 0;
    for (const auto& [delta_file, delta_rank] : directions) {
        int file = file_of(square) + delta_file;
        int rank = rank_of(square) + delta_rank;

        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const int target = rank * 8 + file;
            attacks |= square_bb(target);
            if ((occupancy & square_bb(target)) != 0) {
                break;
            }

            file += delta_file;
            rank += delta_rank;
        }
    }
    return attacks;
}

} // namespace

Bitboard pawn_attacks(Color color, int square)
{
    if (!is_valid_square(square)) {
        return 0;
    }

    Bitboard attacks = 0;
    const int file = file_of(square);
    const int rank = rank_of(square);
    const int target_rank = color == Color::White ? rank + 1 : rank - 1;
    if (target_rank < 0 || target_rank >= 8) {
        return 0;
    }

    if (file > 0) {
        attacks |= square_bb(target_rank * 8 + file - 1);
    }
    if (file < 7) {
        attacks |= square_bb(target_rank * 8 + file + 1);
    }
    return attacks;
}

Bitboard knight_attacks(int square)
{
    return leaper_attacks(square, knight_offsets);
}

Bitboard bishop_attacks(int square, Bitboard occupancy)
{
    return ray_attacks(square, occupancy, bishop_directions);
}

Bitboard rook_attacks(int square, Bitboard occupancy)
{
    return ray_attacks(square, occupancy, rook_directions);
}

Bitboard queen_attacks(int square, Bitboard occupancy)
{
    return bishop_attacks(square, occupancy) | rook_attacks(square, occupancy);
}

Bitboard king_attacks(int square)
{
    return leaper_attacks(square, king_offsets);
}

Bitboard attacks_from(PieceType type, Color color, int square, Bitboard occupancy)
{
    switch (type) {
    case PieceType::Pawn:
        return pawn_attacks(color, square);
    case PieceType::Knight:
        return knight_attacks(square);
    case PieceType::Bishop:
        return bishop_attacks(square, occupancy);
    case PieceType::Rook:
        return rook_attacks(square, occupancy);
    case PieceType::Queen:
        return queen_attacks(square, occupancy);
    case PieceType::King:
        return king_attacks(square);
    case PieceType::None:
        return 0;
    }

    return 0;
}

Bitboard attackers_to(const Board& board, int square, Color color, Bitboard occupancy)
{
    if (!is_valid_square(square)) {
        return 0;
    }

    const Bitboard pawns = board.pieces(color, PieceType::Pawn) & occupancy;
    const Bitboard knights = board.pieces(color, PieceType::Knight) & occupancy;
    const Bitboard bishops = board.pieces(color, PieceType::Bishop) & occupancy;
    const Bitboard rooks = board.pieces(color, PieceType::Rook) & occupancy;
    const Bitboard queens = board.pieces(color, PieceType::Queen) & occupancy;
    const Bitboard king = board.pieces(color, PieceType::King) & occupancy;

    return (pawn_attacks(opposite(color), square) & pawns)
        | (knight_attacks(square) & knights)
        | (bishop_attacks(square, occupancy) & (bishops | queens))
        | (rook_attacks(square, occupancy) & (rooks | queens))
        | (king_attacks(square) & king);
}

} // namespace aetherz
