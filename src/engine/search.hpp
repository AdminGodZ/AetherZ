#pragma once

#include "engine/board.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aetherz {

struct SearchResult {
    std::optional<Move> best_move;
    int score = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
    std::vector<Move> principal_variation;
};

class Search {
public:
    [[nodiscard]] SearchResult find_best_move(Board& board, int depth);

private:
    std::uint64_t nodes_ = 0;

    [[nodiscard]] int negamax(Board& board, int depth, int ply, int alpha, int beta);
    [[nodiscard]] int quiescence(Board& board, int ply, int alpha, int beta);
    [[nodiscard]] int evaluate(const Board& board) const;
    [[nodiscard]] std::vector<Move> ordered_moves(const Board& board) const;
};

[[nodiscard]] std::string pv_to_string(const std::vector<Move>& moves);

} // namespace aetherz
