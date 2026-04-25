#pragma once

#include "engine/board.hpp"

#include <cstdint>
#include <vector>

namespace aetherz {

struct PerftLine {
    Move move;
    std::uint64_t nodes = 0;
};

[[nodiscard]] std::uint64_t perft(Board& board, int depth);
[[nodiscard]] std::vector<PerftLine> perft_divide(Board& board, int depth);

} // namespace aetherz
