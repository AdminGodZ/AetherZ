#include "engine/syzygy.hpp"

#include "tbprobe.h"

#include <algorithm>
#include <bit>
#include <memory>

namespace aetherz::syzygy {
namespace {

struct Position {
    std::uint64_t white;
    std::uint64_t black;
    std::uint64_t kings;
    std::uint64_t queens;
    std::uint64_t rooks;
    std::uint64_t bishops;
    std::uint64_t knights;
    std::uint64_t pawns;
    unsigned rule50;
    unsigned castling;
    unsigned en_passant;
    bool white_to_move;
};

// Fathom uses the same square numbering as the engine: a1 = 0, h8 = 63.
Position position_of(const Board& board)
{
    auto both = [&board](PieceType type) {
        return board.pieces(Color::White, type) | board.pieces(Color::Black, type);
    };
    const int en_passant = board.en_passant_square();
    return Position{
        .white = board.occupancy(Color::White),
        .black = board.occupancy(Color::Black),
        .kings = both(PieceType::King),
        .queens = both(PieceType::Queen),
        .rooks = both(PieceType::Rook),
        .bishops = both(PieceType::Bishop),
        .knights = both(PieceType::Knight),
        .pawns = both(PieceType::Pawn),
        .rule50 = static_cast<unsigned>(board.halfmove_clock()),
        .castling = board.castling_rights(),
        .en_passant = en_passant >= 0 ? static_cast<unsigned>(en_passant) : 0U,
        .white_to_move = board.side_to_move() == Color::White,
    };
}

PieceType promotion_of(unsigned promotes)
{
    switch (promotes) {
    case TB_PROMOTES_QUEEN:
        return PieceType::Queen;
    case TB_PROMOTES_ROOK:
        return PieceType::Rook;
    case TB_PROMOTES_BISHOP:
        return PieceType::Bishop;
    case TB_PROMOTES_KNIGHT:
        return PieceType::Knight;
    default:
        return PieceType::None;
    }
}

} // namespace

bool init(const std::string& path)
{
    if (path.empty() || path == "<empty>") {
        tb_free();
        return true;
    }
    return tb_init(path.c_str());
}

int max_pieces()
{
    return static_cast<int>(TB_LARGEST);
}

std::optional<Wdl> probe_wdl(const Board& board)
{
    if (std::popcount(board.occupancy()) > max_pieces()) {
        return std::nullopt;
    }

    const Position position = position_of(board);
    const unsigned result = tb_probe_wdl(
        position.white,
        position.black,
        position.kings,
        position.queens,
        position.rooks,
        position.bishops,
        position.knights,
        position.pawns,
        position.rule50,
        position.castling,
        position.en_passant,
        position.white_to_move);
    if (result == TB_RESULT_FAILED) {
        return std::nullopt;
    }
    return static_cast<Wdl>(static_cast<int>(result) - 2);
}

std::optional<RootProbe> probe_root(const Board& board, bool has_repeated, bool use_rule50)
{
    if (std::popcount(board.occupancy()) > max_pieces() || board.castling_rights() != 0) {
        return std::nullopt;
    }

    const Position position = position_of(board);
    // Large (each move carries a PV), so not on the stack.
    auto results = std::make_unique<TbRootMoves>();
    RootProbe probe;
    probe.dtz = tb_probe_root_dtz(
                    position.white,
                    position.black,
                    position.kings,
                    position.queens,
                    position.rooks,
                    position.bishops,
                    position.knights,
                    position.pawns,
                    position.rule50,
                    position.castling,
                    position.en_passant,
                    position.white_to_move,
                    has_repeated,
                    use_rule50,
                    results.get())
        != 0;
    if (!probe.dtz) {
        *results = {};
        const int ok = tb_probe_root_wdl(
            position.white,
            position.black,
            position.kings,
            position.queens,
            position.rooks,
            position.bishops,
            position.knights,
            position.pawns,
            position.rule50,
            position.castling,
            position.en_passant,
            position.white_to_move,
            use_rule50,
            results.get());
        if (ok == 0) {
            return std::nullopt;
        }
    }

    const MoveList legal = board.generate_legal_moves();
    for (unsigned index = 0; index < results->size; ++index) {
        const TbRootMove& root_move = results->moves[index];
        const int from = static_cast<int>(TB_MOVE_FROM(root_move.move));
        const int to = static_cast<int>(TB_MOVE_TO(root_move.move));
        const PieceType promotion = promotion_of(TB_MOVE_PROMOTES(root_move.move));
        const auto match = std::ranges::find_if(legal, [&](const Move& move) {
            return move.from == from && move.to == to && move.promotion == promotion;
        });
        if (match == legal.end()) {
            return std::nullopt;
        }
        probe.moves.push_back(RootMove{.move = *match, .rank = root_move.tbRank, .score = root_move.tbScore});
    }
    if (probe.moves.size() != legal.size()) {
        return std::nullopt;
    }

    std::ranges::stable_sort(probe.moves, [](const RootMove& left, const RootMove& right) {
        return left.rank > right.rank;
    });
    return probe;
}

} // namespace aetherz::syzygy
