#include "engine/perft.hpp"

namespace aetherz {

std::uint64_t perft(Board& board, int depth)
{
    if (depth <= 0) {
        return 1;
    }

    const auto moves = board.generate_legal_moves();
    if (depth == 1) {
        return moves.size();
    }

    std::uint64_t nodes = 0;
    for (const Move& move : moves) {
        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }
        nodes += perft(board, depth - 1);
        board.unmake_move(undo);
    }

    return nodes;
}

std::vector<PerftLine> perft_divide(Board& board, int depth)
{
    std::vector<PerftLine> lines;
    const auto moves = board.generate_legal_moves();
    lines.reserve(moves.size());

    for (const Move& move : moves) {
        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }
        lines.push_back(PerftLine{.move = move, .nodes = perft(board, depth - 1)});
        board.unmake_move(undo);
    }

    return lines;
}

} // namespace aetherz
