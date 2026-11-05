#pragma once

#include "engine/board.hpp"
#include "engine/nnue/features.hpp"
#include "engine/nnue/network.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace aetherz::nnue {

// Deeper than any search line, so that search never has to allocate.
inline constexpr int accumulator_stack_size = 160;

// Evaluates positions with a shared, immutable network and keeps its own
// stack of accumulators, one entry per ply. Without a network it does
// nothing; the caller falls back to the classical evaluation.
class NnueEvaluator {
public:
    [[nodiscard]] bool load(std::string_view path, std::string* error = nullptr);
    void clear();
    // Uses other's network (without copying the weights) and a stack of its
    // own, so that each search thread can update accumulators independently.
    void share_network(const NnueEvaluator& other);

    void refresh(const Board& board);
    void push(const DirtyMove& dirty);
    void pop();

    [[nodiscard]] bool loaded() const { return network_ != nullptr; }
    [[nodiscard]] const std::string& eval_file() const { return path_; }
    // Uses the accumulators on top of the stack, which must match board.
    [[nodiscard]] int evaluate(const Board& board) const;
    // Builds the accumulators for board from scratch.
    [[nodiscard]] int evaluate_from_scratch(const Board& board) const;

private:
    using AccumulatorPair = std::array<Accumulator, 2>;
    using AccumulatorStack = std::array<AccumulatorPair, accumulator_stack_size>;

    void build(const Board& board, AccumulatorPair& accumulators) const;
    [[nodiscard]] int evaluate(Color side_to_move, const AccumulatorPair& accumulators) const;

    std::shared_ptr<const Network> network_;
    std::string path_;
    std::unique_ptr<AccumulatorStack> stack_;
    // Index of the current position's accumulators; -1 before the first
    // refresh.
    int top_ = -1;
};

} // namespace aetherz::nnue
