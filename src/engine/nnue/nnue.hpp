#pragma once

#include "engine/board.hpp"
#include "engine/nnue/accumulator.hpp"
#include "engine/nnue/features.hpp"
#include "engine/nnue/network.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aetherz::nnue {

class NnueEvaluator {
public:
    [[nodiscard]] bool load(std::string_view path, std::string* error = nullptr);
    void clear();

    void refresh(const Board& board);
    void push(const Board& board, const DirtyMove& dirty);
    void pop();

    [[nodiscard]] bool loaded() const { return network_.loaded(); }
    [[nodiscard]] bool ready() const { return network_.ready(); }
    [[nodiscard]] const std::string& eval_file() const { return network_.path(); }
    [[nodiscard]] const NnueNetworkMetadata& metadata() const { return network_.metadata(); }
    [[nodiscard]] int evaluate(const Board& board) const;

private:
    [[nodiscard]] AccumulatorState build_accumulator(const Board& board) const;
    [[nodiscard]] AccumulatorState update_accumulator(const Board& board, const DirtyMove& dirty) const;
    void apply_feature(std::vector<std::int32_t>& accumulator, int feature, int sign) const;

    NnueNetwork network_;
    AccumulatorStack accumulator_;
};

} // namespace aetherz::nnue
