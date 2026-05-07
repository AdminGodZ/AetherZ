#include "engine/nnue/nnue.hpp"

#include "engine/evaluate.hpp"

#include <algorithm>

namespace aetherz::nnue {
namespace {

bool king_moved(const DirtyMove& dirty, Color perspective)
{
    for (int index = 0; index < dirty.removed_count; ++index) {
        const Piece piece = dirty.removed[index].piece;
        if (piece != Piece::None && piece_type(piece) == PieceType::King && piece_color(piece) == perspective) {
            return true;
        }
    }
    for (int index = 0; index < dirty.added_count; ++index) {
        const Piece piece = dirty.added[index].piece;
        if (piece != Piece::None && piece_type(piece) == PieceType::King && piece_color(piece) == perspective) {
            return true;
        }
    }

    return false;
}

} // namespace

bool NnueEvaluator::load(std::string_view path, std::string* error)
{
    accumulator_.clear();
    return network_.load(path, error);
}

void NnueEvaluator::clear()
{
    accumulator_.clear();
    network_.clear();
}

void NnueEvaluator::refresh(const Board& board)
{
    if (!network_.ready()) {
        accumulator_.clear();
        return;
    }

    accumulator_.refresh(build_accumulator(board));
}

void NnueEvaluator::push(const Board& board, const DirtyMove& dirty)
{
    if (!network_.ready()) {
        return;
    }

    accumulator_.push(update_accumulator(board, dirty));
}

void NnueEvaluator::pop()
{
    accumulator_.pop();
}

int NnueEvaluator::evaluate(const Board& board) const
{
    if (!network_.ready()) {
        return evaluate_side_to_move(board);
    }

    if (const AccumulatorState* current = accumulator_.current();
        current != nullptr && current->hash == board.hash()) {
        return network_.evaluate(board.side_to_move(), *current);
    }

    const AccumulatorState refreshed = build_accumulator(board);
    return network_.evaluate(board.side_to_move(), refreshed);
}

AccumulatorState NnueEvaluator::build_accumulator(const Board& board) const
{
    AccumulatorState state;
    state.white = network_.accumulator_bias();
    state.black = network_.accumulator_bias();
    state.hash = board.hash();
    state.valid = true;

    for (const int feature : active_halfka_features(board, Color::White)) {
        apply_feature(state.white, feature, 1);
    }
    for (const int feature : active_halfka_features(board, Color::Black)) {
        apply_feature(state.black, feature, 1);
    }

    return state;
}

AccumulatorState NnueEvaluator::update_accumulator(const Board& board, const DirtyMove& dirty) const
{
    const AccumulatorState* current = accumulator_.current();
    if (current == nullptr || static_cast<int>(current->white.size()) != network_.accumulator_size()) {
        return build_accumulator(board);
    }

    AccumulatorState next = *current;
    next.hash = board.hash();
    next.valid = true;

    const auto update_perspective = [&](Color perspective, std::vector<std::int32_t>& values) {
        if (king_moved(dirty, perspective)) {
            values = network_.accumulator_bias();
            for (const int feature : active_halfka_features(board, perspective)) {
                apply_feature(values, feature, 1);
            }
            return;
        }

        const int king = board.king_square(perspective);
        for (int index = 0; index < dirty.removed_count; ++index) {
            const auto [piece, square] = dirty.removed[index];
            apply_feature(values, halfka_feature_index(perspective, king, piece, square), -1);
        }
        for (int index = 0; index < dirty.added_count; ++index) {
            const auto [piece, square] = dirty.added[index];
            apply_feature(values, halfka_feature_index(perspective, king, piece, square), 1);
        }
    };

    update_perspective(Color::White, next.white);
    update_perspective(Color::Black, next.black);
    return next;
}

void NnueEvaluator::apply_feature(std::vector<std::int32_t>& accumulator, int feature, int sign) const
{
    if (feature < 0) {
        return;
    }

    const auto* weights = network_.feature_weights(feature);
    if (weights == nullptr || weights->size() != accumulator.size()) {
        return;
    }

    for (std::size_t index = 0; index < accumulator.size(); ++index) {
        accumulator[index] += sign * (*weights)[index];
    }
}

} // namespace aetherz::nnue
