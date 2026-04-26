#include "engine/evaluate.hpp"

#include "engine/attacks.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>

namespace aetherz {
namespace {

constexpr int max_phase = 24;

constexpr std::array<int, 6> mg_values{
    82,
    337,
    365,
    477,
    1025,
    0,
};

constexpr std::array<int, 6> eg_values{
    94,
    281,
    297,
    512,
    936,
    0,
};

constexpr std::array<int, 6> phase_weights{
    0,
    1,
    1,
    2,
    4,
    0,
};

constexpr std::array<int, 8> passed_pawn_mg{
    0, 5, 10, 20, 35, 60, 100, 0,
};

constexpr std::array<int, 8> passed_pawn_eg{
    0, 10, 20, 40, 70, 110, 170, 0,
};

constexpr std::array<int, 6> mobility_mg{
    0, 4, 4, 2, 1, 0,
};

constexpr std::array<int, 6> mobility_eg{
    0, 3, 4, 4, 2, 0,
};

constexpr std::array<int, 64> mg_pawn{
    0,   0,   0,   0,   0,   0,   0,   0,
    98, 134,  61,  95,  68, 126,  34, -11,
    -6,   7,  26,  31,  65,  56,  25, -20,
   -14,  13,   6,  21,  23,  12,  17, -23,
   -27,  -2,  -5,  12,  17,   6,  10, -25,
   -26,  -4,  -4, -10,   3,   3,  33, -12,
   -35,  -1, -20, -23, -15,  24,  38, -22,
     0,   0,   0,   0,   0,   0,   0,   0,
};

constexpr std::array<int, 64> eg_pawn{
    0,   0,   0,   0,   0,   0,   0,   0,
   178, 173, 158, 134, 147, 132, 165, 187,
    94, 100,  85,  67,  56,  53,  82,  84,
    32,  24,  13,   5,  -2,   4,  17,  17,
    13,   9,  -3,  -7,  -7,  -8,   3,  -1,
     4,   7,  -6,   1,   0,  -5,  -1,  -8,
    13,   8,   8,  10,  13,   0,   2,  -7,
     0,   0,   0,   0,   0,   0,   0,   0,
};

constexpr std::array<int, 64> mg_knight{
   -167, -89, -34, -49,  61, -97, -15, -107,
    -73, -41,  72,  36,  23,  62,   7,  -17,
    -47,  60,  37,  65,  84, 129,  73,   44,
     -9,  17,  19,  53,  37,  69,  18,   22,
    -13,   4,  16,  13,  28,  19,  21,   -8,
    -23,  -9,  12,  10,  19,  17,  25,  -16,
    -29, -53, -12,  -3,  -1,  18, -14,  -19,
   -105, -21, -58, -33, -17, -28, -19,  -23,
};

constexpr std::array<int, 64> eg_knight{
   -58, -38, -13, -28, -31, -27, -63, -99,
   -25,  -8, -25,  -2,  -9, -25, -24, -52,
   -24, -20,  10,   9,  -1,  -9, -19, -41,
   -17,   3,  22,  22,  22,  11,   8, -18,
   -18,  -6,  16,  25,  16,  17,   4, -18,
   -23,  -3,  -1,  15,  10,  -3, -20, -22,
   -42, -20, -10,  -5,  -2, -20, -23, -44,
   -29, -51, -23, -15, -22, -18, -50, -64,
};

constexpr std::array<int, 64> mg_bishop{
   -29,   4, -82, -37, -25, -42,   7,  -8,
   -26,  16, -18, -13,  30,  59,  18, -47,
   -16,  37,  43,  40,  35,  50,  37,  -2,
    -4,   5,  19,  50,  37,  37,   7,  -2,
    -6,  13,  13,  26,  34,  12,  10,   4,
     0,  15,  15,  15,  14,  27,  18,  10,
     4,  15,  16,   0,   7,  21,  33,   1,
   -33,  -3, -14, -21, -13, -12, -39, -21,
};

constexpr std::array<int, 64> eg_bishop{
   -14, -21, -11,  -8,  -7,  -9, -17, -24,
    -8,  -4,   7, -12,  -3, -13,  -4, -14,
     2,  -8,   0,  -1,  -2,   6,   0,   4,
    -3,   9,  12,   9,  14,  10,   3,   2,
    -6,   3,  13,  19,   7,  10,  -3,  -9,
   -12,  -3,   8,  10,  13,   3,  -7, -15,
   -14, -18,  -7,  -1,   4,  -9, -15, -27,
   -23,  -9, -23,  -5,  -9, -16,  -5, -17,
};

constexpr std::array<int, 64> mg_rook{
    32,  42,  32,  51,  63,   9,  31,  43,
    27,  32,  58,  62,  80,  67,  26,  44,
    -5,  19,  26,  36,  17,  45,  61,  16,
   -24, -11,   7,  26,  24,  35,  -8, -20,
   -36, -26, -12,  -1,   9,  -7,   6, -23,
   -45, -25, -16, -17,   3,   0,  -5, -33,
   -44, -16, -20,  -9,  -1,  11,  -6, -71,
   -19, -13,   1,  17,  16,   7, -37, -26,
};

constexpr std::array<int, 64> eg_rook{
    13,  10,  18,  15,  12,  12,   8,   5,
    11,  13,  13,  11,  -3,   3,   8,   3,
     7,   7,   7,   5,   4,  -3,  -5,  -3,
     4,   3,  13,   1,   2,   1,  -1,   2,
     3,   5,   8,   4,  -5,  -6,  -8, -11,
    -4,   0,  -5,  -1,  -7, -12,  -8, -16,
    -6,  -6,   0,   2,  -9,  -9, -11,  -3,
    -9,   2,   3,  -1,  -5, -13,   4, -20,
};

constexpr std::array<int, 64> mg_queen{
   -28,   0,  29,  12,  59,  44,  43,  45,
   -24, -39,  -5,   1, -16,  57,  28,  54,
   -13, -17,   7,   8,  29,  56,  47,  57,
   -27, -27, -16, -16,  -1,  17,  -2,   1,
    -9, -26,  -9, -10,  -2,  -4,   3,  -3,
   -14,   2, -11,  -2,  -5,   2,  14,   5,
   -35,  -8,  11,   2,   8,  15,  -3,   1,
    -1, -18,  -9,  10, -15, -25, -31, -50,
};

constexpr std::array<int, 64> eg_queen{
    -9,  22,  22,  27,  27,  19,  10,  20,
   -17,  20,  32,  41,  58,  25,  30,   0,
   -20,   6,   9,  49,  47,  35,  19,   9,
     3,  22,  24,  45,  57,  40,  57,  36,
   -18,  28,  19,  47,  31,  34,  39,  23,
   -16, -27,  15,   6,   9,  17,  10,   5,
   -22, -23, -30, -16, -16, -23, -36, -32,
   -33, -28, -22, -43,  -5, -32, -20, -41,
};

constexpr std::array<int, 64> mg_king{
   -65,  23,  16, -15, -56, -34,   2,  13,
    29,  -1, -20,  -7,  -8,  -4, -38, -29,
    -9,  24,   2, -16, -20,   6,  22, -22,
   -17, -20, -12, -27, -30, -25, -14, -36,
   -49,  -1, -27, -39, -46, -44, -33, -51,
   -14, -14, -22, -46, -44, -30, -15, -27,
     1,   7,  -8, -64, -43, -16,   9,   8,
   -15,  36,  12, -54,   8, -28,  24,  14,
};

constexpr std::array<int, 64> eg_king{
   -74, -35, -18, -18, -11,  15,   4, -17,
   -12,  17,  14,  17,  17,  38,  23,  11,
    10,  17,  23,  15,  20,  45,  44,  13,
    -8,  22,  24,  27,  26,  33,  26,   3,
   -18,  -4,  21,  24,  27,  23,   9, -11,
   -19,  -3,  11,  21,  23,  16,   7,  -9,
   -27, -11,   4,  13,  14,   4,  -5, -17,
   -53, -34, -21, -11, -28, -14, -24, -43,
};

constexpr std::array<std::array<int, 64>, 6> mg_tables{
    mg_pawn,
    mg_knight,
    mg_bishop,
    mg_rook,
    mg_queen,
    mg_king,
};

constexpr std::array<std::array<int, 64>, 6> eg_tables{
    eg_pawn,
    eg_knight,
    eg_bishop,
    eg_rook,
    eg_queen,
    eg_king,
};

int pop_lsb(Bitboard& bitboard)
{
    const int square = std::countr_zero(bitboard);
    bitboard &= bitboard - 1;
    return square;
}

int mirror_square(int square)
{
    return square ^ 56;
}

Bitboard file_mask(int file)
{
    Bitboard mask = 0;
    for (int rank = 0; rank < 8; ++rank) {
        mask |= square_bb(rank * 8 + file);
    }
    return mask;
}

int advancement(Color color, int square)
{
    return color == Color::White ? rank_of(square) : 7 - rank_of(square);
}

int distance_between(int left, int right)
{
    return std::max(std::abs(file_of(left) - file_of(right)), std::abs(rank_of(left) - rank_of(right)));
}

struct EvalScore {
    int middlegame = 0;
    int endgame = 0;

    void add(int mg, int eg)
    {
        middlegame += mg;
        endgame += eg;
    }
};

struct AttackMaps {
    std::array<Bitboard, 2> attacks{};
    std::array<Bitboard, 2> pawn_attacks{};
};

bool is_passed_pawn(const Board& board, Color color, int square)
{
    const Color enemy = opposite(color);
    const int file = file_of(square);
    const int rank = rank_of(square);
    Bitboard enemy_pawns = board.pieces(enemy, PieceType::Pawn);

    while (enemy_pawns != 0) {
        const int enemy_square = pop_lsb(enemy_pawns);
        if (std::abs(file_of(enemy_square) - file) > 1) {
            continue;
        }

        if (color == Color::White && rank_of(enemy_square) > rank) {
            return false;
        }
        if (color == Color::Black && rank_of(enemy_square) < rank) {
            return false;
        }
    }

    return true;
}

EvalScore evaluate_pawns(const Board& board, Color color)
{
    EvalScore score;
    std::array<int, 8> file_counts{};
    Bitboard pawns = board.pieces(color, PieceType::Pawn);
    Bitboard pawn_copy = pawns;

    while (pawn_copy != 0) {
        ++file_counts[file_of(pop_lsb(pawn_copy))];
    }

    for (const int count : file_counts) {
        if (count > 1) {
            score.add(-12 * (count - 1), -18 * (count - 1));
        }
    }

    while (pawns != 0) {
        const int square = pop_lsb(pawns);
        const int file = file_of(square);
        const bool has_left_neighbor = file > 0 && file_counts[file - 1] > 0;
        const bool has_right_neighbor = file < 7 && file_counts[file + 1] > 0;

        if (!has_left_neighbor && !has_right_neighbor) {
            score.add(-10, -12);
        }

        if (is_passed_pawn(board, color, square)) {
            const int index = std::clamp(advancement(color, square), 0, 7);
            score.add(passed_pawn_mg[index], passed_pawn_eg[index]);
        }
    }

    return score;
}

EvalScore evaluate_mobility(const Board& board, Color color)
{
    EvalScore score;
    const Bitboard occupancy = board.occupancy();
    const Bitboard own_occupancy = board.occupancy(color);

    for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
        Bitboard pieces = board.pieces(color, type);
        while (pieces != 0) {
            const int square = pop_lsb(pieces);
            const int mobility = std::popcount(attacks_from(type, color, square, occupancy) & ~own_occupancy);
            score.add(mobility * mobility_mg[piece_type_index(type)], mobility * mobility_eg[piece_type_index(type)]);
        }
    }

    return score;
}

EvalScore evaluate_bishop_pair(const Board& board, Color color)
{
    EvalScore score;
    if (std::popcount(board.pieces(color, PieceType::Bishop)) >= 2) {
        score.add(30, 45);
    }
    return score;
}

EvalScore evaluate_rooks(const Board& board, Color color)
{
    EvalScore score;
    Bitboard rooks = board.pieces(color, PieceType::Rook);

    while (rooks != 0) {
        const int square = pop_lsb(rooks);
        const int file_index = file_of(square);
        const Bitboard file = file_mask(file_index);
        const bool own_pawn_on_file = (board.pieces(color, PieceType::Pawn) & file) != 0;
        const bool enemy_pawn_on_file = (board.pieces(opposite(color), PieceType::Pawn) & file) != 0;

        if (!own_pawn_on_file && !enemy_pawn_on_file) {
            score.add(25, 10);
        } else if (!own_pawn_on_file) {
            score.add(12, 6);
        }

        const int rank = rank_of(square);
        if ((color == Color::White && rank == 6) || (color == Color::Black && rank == 1)) {
            score.add(20, 30);
        }
    }

    return score;
}

Bitboard build_pawn_attack_map(const Board& board, Color color)
{
    Bitboard attacks = 0;
    Bitboard pawns = board.pieces(color, PieceType::Pawn);
    while (pawns != 0) {
        attacks |= pawn_attacks(color, pop_lsb(pawns));
    }
    return attacks;
}

Bitboard build_attack_map(const Board& board, Color color, Bitboard pawn_attacks)
{
    const Bitboard occupancy = board.occupancy();
    Bitboard attacks = pawn_attacks;

    for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen, PieceType::King}) {
        Bitboard pieces = board.pieces(color, type);
        while (pieces != 0) {
            attacks |= attacks_from(type, color, pop_lsb(pieces), occupancy);
        }
    }

    return attacks;
}

AttackMaps build_attack_maps(const Board& board)
{
    AttackMaps maps;
    for (Color color : {Color::White, Color::Black}) {
        maps.pawn_attacks[color_index(color)] = build_pawn_attack_map(board, color);
        maps.attacks[color_index(color)] = build_attack_map(board, color, maps.pawn_attacks[color_index(color)]);
    }
    return maps;
}

bool pawn_attacks_square(const AttackMaps& maps, Color color, int square)
{
    return (maps.pawn_attacks[color_index(color)] & square_bb(square)) != 0;
}

EvalScore evaluate_threats(const Board& board, Color color, const AttackMaps& maps)
{
    EvalScore score;
    const Color enemy = opposite(color);
    const Bitboard attacks = maps.attacks[color_index(color)];
    const Bitboard enemy_attacks = maps.attacks[color_index(enemy)];
    const Bitboard pawn_attacks = maps.pawn_attacks[color_index(color)];

    for (PieceType type : {PieceType::Pawn, PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
        Bitboard targets = board.pieces(enemy, type);
        while (targets != 0) {
            const int square = pop_lsb(targets);
            const Bitboard target = square_bb(square);
            const int victim_value = middlegame_piece_value(type);
            if ((attacks & target) == 0) {
                continue;
            }

            if ((enemy_attacks & target) == 0) {
                score.add(std::min(160, victim_value / 4), std::min(90, victim_value / 8));
            }

            if ((pawn_attacks & target) != 0 && type != PieceType::Pawn) {
                score.add(std::min(120, victim_value / 5), std::min(50, victim_value / 10));
            }
        }
    }

    return score;
}

EvalScore evaluate_outposts(const Board& board, Color color, const AttackMaps& maps)
{
    EvalScore score;
    const Color enemy = opposite(color);

    for (PieceType type : {PieceType::Knight, PieceType::Bishop}) {
        Bitboard pieces = board.pieces(color, type);
        while (pieces != 0) {
            const int square = pop_lsb(pieces);
            const int rank = rank_of(square);
            const bool advanced = color == Color::White ? rank >= 3 : rank <= 4;
            if (!advanced || !pawn_attacks_square(maps, color, square) || pawn_attacks_square(maps, enemy, square)) {
                continue;
            }

            const bool central = file_of(square) >= 2 && file_of(square) <= 5;
            if (type == PieceType::Knight) {
                score.add(central ? 32 : 22, 18);
            } else {
                score.add(central ? 20 : 14, 12);
            }
        }
    }

    return score;
}

EvalScore evaluate_king_pressure(const Board& board, Color color)
{
    EvalScore score;
    const int enemy_king = board.king_square(opposite(color));
    if (!is_valid_square(enemy_king)) {
        return score;
    }

    const Bitboard occupancy = board.occupancy();
    const Bitboard king_zone = king_attacks(enemy_king) | square_bb(enemy_king);
    for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
        Bitboard pieces = board.pieces(color, type);
        while (pieces != 0) {
            const int square = pop_lsb(pieces);
            const int distance = distance_between(square, enemy_king);
            const int closeness = std::max(0, 7 - distance);
            const int weight = type == PieceType::Queen ? 6 : (type == PieceType::Rook ? 4 : 5);
            score.add(closeness * weight, closeness * weight / 3);

            if ((attacks_from(type, color, square, occupancy) & king_zone) != 0) {
                score.add(weight * 5, weight);
            }
        }
    }

    return score;
}

int attacks_into_zone(const Board& board, Color color, Bitboard zone)
{
    const Bitboard occupancy = board.occupancy();
    int danger = 0;

    Bitboard pawns = board.pieces(color, PieceType::Pawn);
    while (pawns != 0) {
        danger += 8 * std::popcount(pawn_attacks(color, pop_lsb(pawns)) & zone);
    }

    Bitboard knights = board.pieces(color, PieceType::Knight);
    while (knights != 0) {
        danger += 10 * std::popcount(knight_attacks(pop_lsb(knights)) & zone);
    }

    Bitboard bishops = board.pieces(color, PieceType::Bishop);
    while (bishops != 0) {
        danger += 10 * std::popcount(bishop_attacks(pop_lsb(bishops), occupancy) & zone);
    }

    Bitboard rooks = board.pieces(color, PieceType::Rook);
    while (rooks != 0) {
        danger += 14 * std::popcount(rook_attacks(pop_lsb(rooks), occupancy) & zone);
    }

    Bitboard queens = board.pieces(color, PieceType::Queen);
    while (queens != 0) {
        danger += 18 * std::popcount(queen_attacks(pop_lsb(queens), occupancy) & zone);
    }

    return danger;
}

EvalScore evaluate_king_safety(const Board& board, Color color)
{
    EvalScore score;
    const int king = board.king_square(color);
    if (!is_valid_square(king)) {
        return score;
    }

    const Bitboard zone = king_attacks(king) | square_bb(king);
    const int danger = attacks_into_zone(board, opposite(color), zone);
    score.add(-danger, -danger / 4);

    for (int file_index = std::max(0, file_of(king) - 1); file_index <= std::min(7, file_of(king) + 1); ++file_index) {
        const Bitboard file = file_mask(file_index);
        if ((board.pieces(color, PieceType::Pawn) & file) == 0) {
            score.add(file_index == file_of(king) ? -18 : -8, 0);
        }
    }

    const int shield_rank = color == Color::White ? rank_of(king) + 1 : rank_of(king) - 1;
    if (shield_rank < 0 || shield_rank >= 8) {
        return score;
    }

    for (int file = std::max(0, file_of(king) - 1); file <= std::min(7, file_of(king) + 1); ++file) {
        const Piece shield_piece = board.piece_at(shield_rank * 8 + file);
        if (shield_piece == make_piece(color, PieceType::Pawn)) {
            score.add(8, 0);
        } else {
            score.add(-4, 0);
        }
    }

    return score;
}

EvalScore evaluate_castling(const Board& board, Color color)
{
    EvalScore score;
    const int king = board.king_square(color);
    if (!is_valid_square(king)) {
        return score;
    }

    const bool castled = color == Color::White
        ? (king == 6 || king == 2)
        : (king == 62 || king == 58);
    const int start_square = color == Color::White ? 4 : 60;
    const bool has_castling_rights = board.can_castle(color, true) || board.can_castle(color, false);

    if (castled) {
        score.add(110, 15);
        return score;
    }

    if (king == start_square) {
        score.add(has_castling_rights ? 20 : -35, 0);
        return score;
    }

    const int file = file_of(king);
    const int home_rank = color == Color::White ? 0 : 7;
    const int near_home_rank = color == Color::White ? 1 : 6;
    const bool early_king_walk = rank_of(king) == home_rank || rank_of(king) == near_home_rank;

    if (early_king_walk) {
        score.add(-125, -10);
    }
    if (file >= 2 && file <= 5) {
        score.add(-35, -5);
    }

    return score;
}

EvalScore evaluate_positional_terms(const Board& board, Color color, const AttackMaps& maps)
{
    EvalScore score;

    for (const EvalScore term : {
             evaluate_pawns(board, color),
             evaluate_mobility(board, color),
             evaluate_bishop_pair(board, color),
             evaluate_rooks(board, color),
             evaluate_threats(board, color, maps),
             evaluate_outposts(board, color, maps),
             evaluate_king_pressure(board, color),
             evaluate_king_safety(board, color),
             evaluate_castling(board, color),
         }) {
        score.middlegame += term.middlegame;
        score.endgame += term.endgame;
    }

    return score;
}

} // namespace

int middlegame_piece_value(PieceType type)
{
    if (type == PieceType::None) {
        return 0;
    }

    return mg_values[piece_type_index(type)];
}

int endgame_piece_value(PieceType type)
{
    if (type == PieceType::None) {
        return 0;
    }

    return eg_values[piece_type_index(type)];
}

int evaluate_white(const Board& board)
{
    int middlegame = 0;
    int endgame = 0;
    int phase = 0;

    for (int type_index = 0; type_index < 6; ++type_index) {
        const auto type = static_cast<PieceType>(type_index);

        Bitboard white_pieces = board.pieces(Color::White, type);
        while (white_pieces != 0) {
            const int square = pop_lsb(white_pieces);
            middlegame += mg_values[type_index] + mg_tables[type_index][square];
            endgame += eg_values[type_index] + eg_tables[type_index][square];
            phase += phase_weights[type_index];
        }

        Bitboard black_pieces = board.pieces(Color::Black, type);
        while (black_pieces != 0) {
            const int square = mirror_square(pop_lsb(black_pieces));
            middlegame -= mg_values[type_index] + mg_tables[type_index][square];
            endgame -= eg_values[type_index] + eg_tables[type_index][square];
            phase += phase_weights[type_index];
        }
    }

    const AttackMaps attack_maps = build_attack_maps(board);
    const EvalScore white_terms = evaluate_positional_terms(board, Color::White, attack_maps);
    const EvalScore black_terms = evaluate_positional_terms(board, Color::Black, attack_maps);
    middlegame += white_terms.middlegame - black_terms.middlegame;
    endgame += white_terms.endgame - black_terms.endgame;

    phase = std::clamp(phase, 0, max_phase);
    return (middlegame * phase + endgame * (max_phase - phase)) / max_phase;
}

int evaluate_side_to_move(const Board& board)
{
    const int score = evaluate_white(board);
    return board.side_to_move() == Color::White ? score : -score;
}

} // namespace aetherz
