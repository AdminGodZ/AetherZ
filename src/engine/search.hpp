#pragma once

#include "engine/board.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
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

struct SearchLimits {
    int max_depth = 4;
    std::optional<std::chrono::milliseconds> move_time;
    std::function<void(const SearchResult&)> on_depth_finished;
};

class Search {
public:
    [[nodiscard]] SearchResult find_best_move(Board& board, int depth);
    [[nodiscard]] SearchResult find_best_move(Board& board, const SearchLimits& limits);

private:
    std::uint64_t nodes_ = 0;
    bool stop_ = false;
    std::optional<std::chrono::steady_clock::time_point> deadline_;

    [[nodiscard]] SearchResult search_depth(Board& board, int depth);
    [[nodiscard]] int negamax(Board& board, int depth, int ply, int alpha, int beta);
    [[nodiscard]] int quiescence(Board& board, int ply, int alpha, int beta);
    [[nodiscard]] int evaluate(const Board& board) const;
    [[nodiscard]] std::vector<Move> ordered_moves(const Board& board) const;
    [[nodiscard]] bool should_stop(bool force_check = false);
};

[[nodiscard]] std::string pv_to_string(const std::vector<Move>& moves);

} // namespace aetherz
