#pragma once

#include "engine/board.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

struct EpdPosition {
    std::string fen;
    std::vector<std::string> best_moves;
    std::vector<std::string> avoid_moves;
    std::string id;
};

[[nodiscard]] std::optional<EpdPosition> parse_epd(std::string_view text);
[[nodiscard]] std::vector<EpdPosition> load_epd_file(std::string_view path);
[[nodiscard]] bool epd_move_matches(const Board& board, std::string_view expected, const Move& move);

} // namespace aetherz
