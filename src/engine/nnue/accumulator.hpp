#pragma once

#include <cstdint>
#include <vector>

namespace aetherz::nnue {

struct AccumulatorState {
    std::vector<std::int32_t> white;
    std::vector<std::int32_t> black;
    std::uint64_t hash = 0;
    bool valid = false;
};

class AccumulatorStack {
public:
    void clear()
    {
        stack_.clear();
    }

    void refresh(AccumulatorState state)
    {
        stack_.clear();
        state.valid = true;
        stack_.push_back(state);
    }

    void push(AccumulatorState state)
    {
        state.valid = true;
        stack_.push_back(state);
    }

    void pop()
    {
        if (!stack_.empty()) {
            stack_.pop_back();
        }
    }

    [[nodiscard]] bool valid() const
    {
        return !stack_.empty() && stack_.back().valid;
    }

    [[nodiscard]] const AccumulatorState* current() const
    {
        if (!valid()) {
            return nullptr;
        }

        return &stack_.back();
    }

private:
    std::vector<AccumulatorState> stack_;
};

} // namespace aetherz::nnue
