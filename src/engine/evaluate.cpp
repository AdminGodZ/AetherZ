#include "engine/evaluate.hpp"

#include "engine/attacks.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>

namespace aetherz {
namespace {

constexpr EvalWeights weights{{
#include "engine/eval_weights.inc"
}};

constexpr std::array<int, 6> see_values{82, 337, 365, 477, 1025, 0};
constexpr std::array<int, 6> endgame_values{94, 281, 297, 512, 936, 0};
constexpr std::array<int, 6> phase_weights{0, 1, 1, 2, 4, 0};

int pop_lsb(Bitboard& bitboard)
{
    const int square = std::countr_zero(bitboard);
    bitboard &= bitboard - 1;
    return square;
}

constexpr Bitboard file_mask(int file)
{
    return Bitboard{0x0101010101010101ULL} << file;
}

// A square as seen from the given side, so that a1 is that side's corner.
constexpr int relative_square(Color color, int square)
{
    return color == Color::White ? square : square ^ 56;
}

int advancement(Color color, int square)
{
    return color == Color::White ? rank_of(square) : 7 - rank_of(square);
}

int distance_between(int left, int right)
{
    return std::max(std::abs(file_of(left) - file_of(right)), std::abs(rank_of(left) - rank_of(right)));
}

// Squares on the same and adjacent files strictly in front of a pawn: the
// pawn is passed when no enemy pawn stands on them.
constexpr std::array<std::array<Bitboard, 64>, 2> passed_pawn_masks = [] {
    std::array<std::array<Bitboard, 64>, 2> masks{};
    for (int square = 0; square < 64; ++square) {
        const int file = file_of(square);
        const int rank = rank_of(square);
        Bitboard files = file_mask(file);
        if (file > 0) {
            files |= file_mask(file - 1);
        }
        if (file < 7) {
            files |= file_mask(file + 1);
        }

        Bitboard ahead_white = 0;
        Bitboard ahead_black = 0;
        for (int other = 0; other < 8; ++other) {
            const Bitboard rank_squares = Bitboard{0xFF} << (8 * other);
            if (other > rank) {
                ahead_white |= rank_squares;
            }
            if (other < rank) {
                ahead_black |= rank_squares;
            }
        }
        masks[0][square] = files & ahead_white;
        masks[1][square] = files & ahead_black;
    }
    return masks;
}();

struct AttackMaps {
    std::array<Bitboard, 2> attacks{};
    std::array<Bitboard, 2> pawn_attacks{};
};

struct NoTrace {
    void add(int, Color, int) {}
};

struct CoefficientTrace {
    EvalTrace& trace;

    void add(int index, Color color, int count)
    {
        trace.coefficients[index] += color == Color::White ? count : -count;
    }
};

// Accumulates weighted terms from white's point of view, and optionally
// records how often each weight was used.
template <typename Trace>
class Evaluator {
public:
    Evaluator(const Board& board, Trace& trace)
        : board_(board)
        , trace_(trace)
    {
    }

    int evaluate(int* phase_out)
    {
        int phase = 0;
        for (const Color color : {Color::White, Color::Black}) {
            for (int type_index = 0; type_index < 6; ++type_index) {
                Bitboard pieces = board_.pieces(color, static_cast<PieceType>(type_index));
                while (pieces != 0) {
                    const int square = relative_square(color, pop_lsb(pieces));
                    add(eval_index::material + type_index, color);
                    add(eval_index::piece_square + type_index * 64 + square, color);
                    phase += phase_weights[type_index];
                }
            }
        }

        build_attack_maps();
        for (const Color color : {Color::White, Color::Black}) {
            evaluate_pawns(color);
            evaluate_mobility(color);
            evaluate_bishop_pair(color);
            evaluate_rooks(color);
            evaluate_threats(color);
            evaluate_outposts(color);
            evaluate_king_pressure(color);
            evaluate_king_safety(color);
            evaluate_castling(color);
        }

        phase = std::clamp(phase, 0, max_eval_phase);
        if (phase_out != nullptr) {
            *phase_out = phase;
        }
        return (middlegame_ * phase + endgame_ * (max_eval_phase - phase)) / max_eval_phase;
    }

private:
    const Board& board_;
    Trace& trace_;
    int middlegame_ = 0;
    int endgame_ = 0;
    AttackMaps maps_;

    void add(int index, Color color, int count = 1)
    {
        const int sign = color == Color::White ? count : -count;
        middlegame_ += sign * weights[index].middlegame;
        endgame_ += sign * weights[index].endgame;
        trace_.add(index, color, count);
    }

    void build_attack_maps()
    {
        const Bitboard occupancy = board_.occupancy();
        for (const Color color : {Color::White, Color::Black}) {
            Bitboard pawn_attack_map = 0;
            Bitboard pawns = board_.pieces(color, PieceType::Pawn);
            while (pawns != 0) {
                pawn_attack_map |= pawn_attacks(color, pop_lsb(pawns));
            }

            Bitboard attacks = pawn_attack_map;
            for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen, PieceType::King}) {
                Bitboard pieces = board_.pieces(color, type);
                while (pieces != 0) {
                    attacks |= attacks_from(type, color, pop_lsb(pieces), occupancy);
                }
            }

            maps_.pawn_attacks[color_index(color)] = pawn_attack_map;
            maps_.attacks[color_index(color)] = attacks;
        }
    }

    [[nodiscard]] bool pawn_attacks_square(Color color, int square) const
    {
        return (maps_.pawn_attacks[color_index(color)] & square_bb(square)) != 0;
    }

    void evaluate_pawns(Color color)
    {
        std::array<int, 8> file_counts{};
        Bitboard pawns = board_.pieces(color, PieceType::Pawn);
        Bitboard pawn_copy = pawns;
        while (pawn_copy != 0) {
            ++file_counts[file_of(pop_lsb(pawn_copy))];
        }

        for (const int count : file_counts) {
            if (count > 1) {
                add(eval_index::doubled_pawn, color, count - 1);
            }
        }

        const Bitboard enemy_pawns = board_.pieces(opposite(color), PieceType::Pawn);
        while (pawns != 0) {
            const int square = pop_lsb(pawns);
            const int file = file_of(square);
            const bool has_left_neighbor = file > 0 && file_counts[file - 1] > 0;
            const bool has_right_neighbor = file < 7 && file_counts[file + 1] > 0;
            if (!has_left_neighbor && !has_right_neighbor) {
                add(eval_index::isolated_pawn, color);
            }

            if ((passed_pawn_masks[color_index(color)][square] & enemy_pawns) == 0) {
                add(eval_index::passed_pawn + std::clamp(advancement(color, square), 0, 7), color);
            }
        }
    }

    void evaluate_mobility(Color color)
    {
        const Bitboard occupancy = board_.occupancy();
        const Bitboard own_occupancy = board_.occupancy(color);
        for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
            Bitboard pieces = board_.pieces(color, type);
            while (pieces != 0) {
                const int square = pop_lsb(pieces);
                const int mobility = std::popcount(attacks_from(type, color, square, occupancy) & ~own_occupancy);
                add(eval_index::mobility + piece_type_index(type), color, mobility);
            }
        }
    }

    void evaluate_bishop_pair(Color color)
    {
        if (std::popcount(board_.pieces(color, PieceType::Bishop)) >= 2) {
            add(eval_index::bishop_pair, color);
        }
    }

    void evaluate_rooks(Color color)
    {
        Bitboard rooks = board_.pieces(color, PieceType::Rook);
        while (rooks != 0) {
            const int square = pop_lsb(rooks);
            const Bitboard file = file_mask(file_of(square));
            const bool own_pawn_on_file = (board_.pieces(color, PieceType::Pawn) & file) != 0;
            const bool enemy_pawn_on_file = (board_.pieces(opposite(color), PieceType::Pawn) & file) != 0;

            if (!own_pawn_on_file && !enemy_pawn_on_file) {
                add(eval_index::rook_open_file, color);
            } else if (!own_pawn_on_file) {
                add(eval_index::rook_semi_open_file, color);
            }

            if (advancement(color, square) == 6) {
                add(eval_index::rook_seventh_rank, color);
            }
        }
    }

    void evaluate_threats(Color color)
    {
        const Color enemy = opposite(color);
        const Bitboard attacks = maps_.attacks[color_index(color)];
        const Bitboard enemy_attacks = maps_.attacks[color_index(enemy)];
        const Bitboard own_pawn_attacks = maps_.pawn_attacks[color_index(color)];

        for (PieceType type : {PieceType::Pawn, PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
            Bitboard targets = board_.pieces(enemy, type) & attacks;
            while (targets != 0) {
                const Bitboard target = square_bb(pop_lsb(targets));
                if ((enemy_attacks & target) == 0) {
                    add(eval_index::threat_hanging + piece_type_index(type), color);
                }
                if ((own_pawn_attacks & target) != 0 && type != PieceType::Pawn) {
                    add(eval_index::threat_by_pawn + piece_type_index(type), color);
                }
            }
        }
    }

    void evaluate_outposts(Color color)
    {
        const Color enemy = opposite(color);
        for (PieceType type : {PieceType::Knight, PieceType::Bishop}) {
            Bitboard pieces = board_.pieces(color, type);
            while (pieces != 0) {
                const int square = pop_lsb(pieces);
                const bool advanced = advancement(color, square) >= 3;
                if (!advanced || !pawn_attacks_square(color, square) || pawn_attacks_square(enemy, square)) {
                    continue;
                }

                const bool central = file_of(square) >= 2 && file_of(square) <= 5;
                if (type == PieceType::Knight) {
                    add(central ? eval_index::outpost_knight_central : eval_index::outpost_knight, color);
                } else {
                    add(central ? eval_index::outpost_bishop_central : eval_index::outpost_bishop, color);
                }
            }
        }
    }

    void evaluate_king_pressure(Color color)
    {
        const int enemy_king = board_.king_square(opposite(color));
        if (!is_valid_square(enemy_king)) {
            return;
        }

        const Bitboard occupancy = board_.occupancy();
        const Bitboard king_zone = king_attacks(enemy_king) | square_bb(enemy_king);
        for (PieceType type : {PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
            Bitboard pieces = board_.pieces(color, type);
            while (pieces != 0) {
                const int square = pop_lsb(pieces);
                const int closeness = std::max(0, 7 - distance_between(square, enemy_king));
                add(eval_index::king_closeness + piece_type_index(type), color, closeness);

                if ((attacks_from(type, color, square, occupancy) & king_zone) != 0) {
                    add(eval_index::king_zone_presence + piece_type_index(type), color);
                }
            }
        }
    }

    void evaluate_king_safety(Color color)
    {
        const int king = board_.king_square(color);
        if (!is_valid_square(king)) {
            return;
        }

        // Squares around the king attacked by each enemy piece type.
        const Color enemy = opposite(color);
        const Bitboard zone = king_attacks(king) | square_bb(king);
        const Bitboard occupancy = board_.occupancy();
        for (PieceType type : {PieceType::Pawn, PieceType::Knight, PieceType::Bishop, PieceType::Rook, PieceType::Queen}) {
            int attacked = 0;
            Bitboard pieces = board_.pieces(enemy, type);
            while (pieces != 0) {
                attacked += std::popcount(attacks_from(type, enemy, pop_lsb(pieces), occupancy) & zone);
            }
            if (attacked != 0) {
                add(eval_index::king_zone_attack + piece_type_index(type), color, attacked);
            }
        }

        const Bitboard own_pawns = board_.pieces(color, PieceType::Pawn);
        for (int file = std::max(0, file_of(king) - 1); file <= std::min(7, file_of(king) + 1); ++file) {
            if ((own_pawns & file_mask(file)) == 0) {
                add(file == file_of(king) ? eval_index::king_open_file : eval_index::king_adjacent_open_file, color);
            }
        }

        const int shield_rank = color == Color::White ? rank_of(king) + 1 : rank_of(king) - 1;
        if (shield_rank < 0 || shield_rank >= 8) {
            return;
        }

        for (int file = std::max(0, file_of(king) - 1); file <= std::min(7, file_of(king) + 1); ++file) {
            const bool shielded = board_.piece_at(shield_rank * 8 + file) == make_piece(color, PieceType::Pawn);
            add(shielded ? eval_index::pawn_shield : eval_index::pawn_shield_missing, color);
        }
    }

    void evaluate_castling(Color color)
    {
        const int king = board_.king_square(color);
        if (!is_valid_square(king)) {
            return;
        }

        const int relative_king = relative_square(color, king);
        if (relative_king == 6 || relative_king == 2) {
            add(eval_index::castled, color);
            return;
        }

        if (relative_king == 4) {
            const bool has_castling_rights = board_.can_castle(color, true) || board_.can_castle(color, false);
            add(has_castling_rights ? eval_index::uncastled_with_rights : eval_index::uncastled_without_rights, color);
            return;
        }

        if (rank_of(relative_king) <= 1) {
            add(eval_index::king_walk, color);
        }
        if (file_of(king) >= 2 && file_of(king) <= 5) {
            add(eval_index::king_central_file, color);
        }
    }
};

} // namespace

const EvalWeights& eval_weights()
{
    return weights;
}

int middlegame_piece_value(PieceType type)
{
    return type == PieceType::None ? 0 : see_values[piece_type_index(type)];
}

int endgame_piece_value(PieceType type)
{
    return type == PieceType::None ? 0 : endgame_values[piece_type_index(type)];
}

int evaluate_white(const Board& board)
{
    NoTrace trace;
    return Evaluator<NoTrace>{board, trace}.evaluate(nullptr);
}

int evaluate_side_to_move(const Board& board)
{
    const int score = evaluate_white(board);
    return board.side_to_move() == Color::White ? score : -score;
}

int evaluate_white_traced(const Board& board, EvalTrace& trace)
{
    trace = EvalTrace{};
    CoefficientTrace recorder{trace};
    return Evaluator<CoefficientTrace>{board, recorder}.evaluate(&trace.phase);
}

} // namespace aetherz
