#include "engine/nnue/nnue.hpp"

#include "engine/evaluate.hpp"

#include <bit>

namespace aetherz::nnue {
namespace {

// Far enough below zero that pushes without a refresh never reach a valid
// stack index.
constexpr int no_accumulators = -(1 << 20);

// out = in + sum(adds) - sum(subs). Plain loops over aligned int16 arrays so
// that the compiler vectorises them; int16 overflow wraps as in Bullet.
template <int Adds, int Subs>
void update(
    std::int16_t* __restrict out,
    const std::int16_t* __restrict in,
    const std::array<const std::int16_t*, 2>& adds,
    const std::array<const std::int16_t*, 2>& subs)
{
    for (int index = 0; index < hidden_size; ++index) {
        int value = in[index];
        for (int add = 0; add < Adds; ++add) {
            value += adds[add][index];
        }
        for (int sub = 0; sub < Subs; ++sub) {
            value -= subs[sub][index];
        }
        out[index] = static_cast<std::int16_t>(value);
    }
}

template <int Sign>
void apply_feature(Accumulator& accumulator, const std::int16_t* weights)
{
    for (int index = 0; index < hidden_size; ++index) {
        accumulator.values[index] = static_cast<std::int16_t>(accumulator.values[index] + Sign * weights[index]);
    }
}

} // namespace

bool NnueEvaluator::load(std::string_view path, std::string* error)
{
    std::unique_ptr<Network> network;
    if (path != embedded_network_name) {
        network = load_network(path, error);
    } else if (embedded_network().empty()) {
        if (error != nullptr) {
            *error = "no network is built into this engine (CMake option AETHERZ_EVALFILE)";
        }
    } else {
        network = parse_network(embedded_network(), path, error);
    }
    if (!network) {
        clear();
        return false;
    }

    network_ = std::move(network);
    path_ = std::string{path};
    if (!stack_) {
        stack_ = std::make_unique<AccumulatorStack>();
    }
    top_ = no_accumulators;
    return true;
}

void NnueEvaluator::clear()
{
    network_.reset();
    path_.clear();
    stack_.reset();
    top_ = no_accumulators;
}

void NnueEvaluator::share_network(const NnueEvaluator& other)
{
    if (!other.network_) {
        clear();
        return;
    }

    network_ = other.network_;
    path_ = other.path_;
    if (!stack_) {
        stack_ = std::make_unique<AccumulatorStack>();
    }
    top_ = no_accumulators;
}

void NnueEvaluator::refresh(const Board& board)
{
    if (!network_) {
        return;
    }

    top_ = 0;
    build(board, (*stack_)[0]);
}

void NnueEvaluator::push(const DirtyMove& dirty)
{
    if (!network_) {
        return;
    }

    ++top_;
    // Too deep (or never refreshed): evaluate() rebuilds from the board.
    if (top_ <= 0 || top_ >= accumulator_stack_size) {
        return;
    }

    const AccumulatorPair& parent = (*stack_)[top_ - 1];
    AccumulatorPair& child = (*stack_)[top_];
    for (const Color perspective : {Color::White, Color::Black}) {
        std::array<const std::int16_t*, 2> adds{};
        std::array<const std::int16_t*, 2> subs{};
        for (int index = 0; index < dirty.added_count; ++index) {
            const auto [piece, square] = dirty.added[index];
            adds[index] = network_->feature_weights[feature_index(perspective, piece, square)].values.data();
        }
        for (int index = 0; index < dirty.removed_count; ++index) {
            const auto [piece, square] = dirty.removed[index];
            subs[index] = network_->feature_weights[feature_index(perspective, piece, square)].values.data();
        }

        const int side = color_index(perspective);
        std::int16_t* out = child[side].values.data();
        const std::int16_t* in = parent[side].values.data();
        switch (dirty.added_count * 4 + dirty.removed_count) {
        case 0:
            update<0, 0>(out, in, adds, subs);
            break;
        case 1 * 4 + 1:
            update<1, 1>(out, in, adds, subs);
            break;
        case 1 * 4 + 2:
            update<1, 2>(out, in, adds, subs);
            break;
        case 2 * 4 + 2:
            update<2, 2>(out, in, adds, subs);
            break;
        default:
            // Not produced by dirty_move_from, but cheap to support.
            child[side] = parent[side];
            for (int index = 0; index < dirty.added_count; ++index) {
                apply_feature<1>(child[side], adds[index]);
            }
            for (int index = 0; index < dirty.removed_count; ++index) {
                apply_feature<-1>(child[side], subs[index]);
            }
            break;
        }
    }
}

void NnueEvaluator::pop()
{
    if (network_) {
        --top_;
    }
}

int NnueEvaluator::evaluate(const Board& board) const
{
    if (!network_) {
        return evaluate_side_to_move(board);
    }
    if (top_ < 0 || top_ >= accumulator_stack_size) {
        return evaluate_from_scratch(board);
    }

    return evaluate(board.side_to_move(), (*stack_)[top_]);
}

int NnueEvaluator::evaluate_from_scratch(const Board& board) const
{
    if (!network_) {
        return evaluate_side_to_move(board);
    }

    AccumulatorPair accumulators;
    build(board, accumulators);
    return evaluate(board.side_to_move(), accumulators);
}

int NnueEvaluator::evaluate(Color side_to_move, const AccumulatorPair& accumulators) const
{
    const int us = color_index(side_to_move);
    return network_->evaluate(accumulators[us], accumulators[us ^ 1]);
}

void NnueEvaluator::build(const Board& board, AccumulatorPair& accumulators) const
{
    auto& white = accumulators[color_index(Color::White)];
    auto& black = accumulators[color_index(Color::Black)];
    white = network_->feature_bias;
    black = network_->feature_bias;

    for (int index = 0; index < 12; ++index) {
        const auto piece = static_cast<Piece>(index + 1);
        for (Bitboard pieces = board.pieces(piece); pieces != 0; pieces &= pieces - 1) {
            const int square = std::countr_zero(pieces);
            apply_feature<1>(white, network_->feature_weights[feature_index(Color::White, piece, square)].values.data());
            apply_feature<1>(black, network_->feature_weights[feature_index(Color::Black, piece, square)].values.data());
        }
    }
}

} // namespace aetherz::nnue
