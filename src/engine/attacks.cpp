#include "engine/attacks.hpp"

#include <array>
#include <bit>
#include <utility>
#include <vector>

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

constexpr Bitboard rank_1 = 0x00000000000000FFULL;
constexpr Bitboard rank_8 = 0xFF00000000000000ULL;
constexpr Bitboard file_a = 0x0101010101010101ULL;
constexpr Bitboard file_h = 0x8080808080808080ULL;

Bitboard leaper_attacks(int square, const std::array<std::pair<int, int>, 8>& offsets)
{
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

// Fancy magic bitboards: the relevant blockers of a slider are multiplied by
// a magic number so that the top bits index a per-square attack table.
struct Magic {
    Bitboard mask = 0;
    Bitboard magic = 0;
    unsigned shift = 0;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t index(Bitboard occupancy) const
    {
        return offset + static_cast<std::size_t>(((occupancy & mask) * magic) >> shift);
    }
};

struct AttackTables {
    std::array<std::array<Bitboard, 64>, 2> pawn{};
    std::array<Bitboard, 64> knight{};
    std::array<Bitboard, 64> king{};
    std::array<Magic, 64> bishop_magics{};
    std::array<Magic, 64> rook_magics{};
    std::vector<Bitboard> bishop_table;
    std::vector<Bitboard> rook_table;
};

std::uint64_t xorshift64(std::uint64_t& state)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}

void init_magics(
    std::array<Magic, 64>& magics,
    std::vector<Bitboard>& table,
    const std::array<std::pair<int, int>, 4>& directions)
{
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    std::vector<Bitboard> occupancies;
    std::vector<Bitboard> references;
    std::vector<int> epoch;
    int attempt = 0;

    for (int square = 0; square < 64; ++square) {
        const Bitboard edges = ((rank_1 | rank_8) & ~(rank_1 << (8 * rank_of(square))))
            | ((file_a | file_h) & ~(file_a << file_of(square)));

        Magic& magic = magics[square];
        magic.mask = ray_attacks(square, 0, directions) & ~edges;
        magic.shift = static_cast<unsigned>(64 - std::popcount(magic.mask));
        magic.offset = table.size();

        occupancies.clear();
        references.clear();
        Bitboard subset = 0;
        do {
            occupancies.push_back(subset);
            references.push_back(ray_attacks(square, subset, directions));
            subset = (subset - magic.mask) & magic.mask;
        } while (subset != 0);

        const std::size_t size = occupancies.size();
        table.resize(table.size() + size);
        epoch.assign(size, 0);

        // Search for a magic that maps every blocker subset without
        // destructive collisions. Sparse random numbers converge quickly.
        while (true) {
            do {
                magic.magic = xorshift64(seed) & xorshift64(seed) & xorshift64(seed);
            } while (std::popcount((magic.mask * magic.magic) >> 56) < 6);

            ++attempt;
            bool ok = true;
            for (std::size_t index = 0; index < size && ok; ++index) {
                const std::size_t slot = magic.index(occupancies[index]) - magic.offset;
                if (epoch[slot] < attempt) {
                    epoch[slot] = attempt;
                    table[magic.offset + slot] = references[index];
                } else if (table[magic.offset + slot] != references[index]) {
                    ok = false;
                }
            }
            if (ok) {
                break;
            }
        }
    }
}

AttackTables build_attack_tables()
{
    AttackTables tables;
    for (int square = 0; square < 64; ++square) {
        tables.knight[square] = leaper_attacks(square, knight_offsets);
        tables.king[square] = leaper_attacks(square, king_offsets);

        const int file = file_of(square);
        const int rank = rank_of(square);
        for (const Color color : {Color::White, Color::Black}) {
            const int target_rank = color == Color::White ? rank + 1 : rank - 1;
            Bitboard attacks = 0;
            if (target_rank >= 0 && target_rank < 8) {
                if (file > 0) {
                    attacks |= square_bb(target_rank * 8 + file - 1);
                }
                if (file < 7) {
                    attacks |= square_bb(target_rank * 8 + file + 1);
                }
            }
            tables.pawn[color_index(color)][square] = attacks;
        }
    }

    init_magics(tables.bishop_magics, tables.bishop_table, bishop_directions);
    init_magics(tables.rook_magics, tables.rook_table, rook_directions);
    return tables;
}

const AttackTables& attack_tables()
{
    static const AttackTables tables = build_attack_tables();
    return tables;
}

// Built during static initialization so the hot path below does not pay for
// a function-local static guard.
const AttackTables& tables = attack_tables();

} // namespace

Bitboard pawn_attacks(Color color, int square)
{
    return tables.pawn[color_index(color)][square];
}

Bitboard knight_attacks(int square)
{
    return tables.knight[square];
}

Bitboard bishop_attacks(int square, Bitboard occupancy)
{
    return tables.bishop_table[tables.bishop_magics[square].index(occupancy)];
}

Bitboard rook_attacks(int square, Bitboard occupancy)
{
    return tables.rook_table[tables.rook_magics[square].index(occupancy)];
}

Bitboard queen_attacks(int square, Bitboard occupancy)
{
    return bishop_attacks(square, occupancy) | rook_attacks(square, occupancy);
}

Bitboard king_attacks(int square)
{
    return tables.king[square];
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

    const Bitboard queens = board.pieces(color, PieceType::Queen);
    const Bitboard bishops = board.pieces(color, PieceType::Bishop) | queens;
    const Bitboard rooks = board.pieces(color, PieceType::Rook) | queens;

    return ((pawn_attacks(opposite(color), square) & board.pieces(color, PieceType::Pawn))
               | (knight_attacks(square) & board.pieces(color, PieceType::Knight))
               | (bishop_attacks(square, occupancy) & bishops)
               | (rook_attacks(square, occupancy) & rooks)
               | (king_attacks(square) & board.pieces(color, PieceType::King)))
        & occupancy;
}

} // namespace aetherz
