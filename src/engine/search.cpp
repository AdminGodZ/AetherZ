#include "engine/search.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <sstream>

namespace aetherz {
namespace {

constexpr int infinity = 32000;
constexpr int mate_score = 30000;

constexpr std::array<int, 6> piece_values{
    100,
    320,
    330,
    500,
    900,
    0,
};

bool is_tactical(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::Promotion
        || move.flag == MoveFlag::PromotionCapture;
}

} // namespace

SearchResult Search::find_best_move(Board& board, int depth)
{
    SearchLimits limits;
    limits.max_depth = depth;
    return find_best_move(board, limits);
}

SearchResult Search::find_best_move(Board& board, const SearchLimits& limits)
{
    nodes_ = 0;
    stop_ = false;
    deadline_.reset();

    if (limits.move_time) {
        deadline_ = std::chrono::steady_clock::now() + *limits.move_time;
    }

    const auto moves = ordered_moves(board);
    if (moves.empty()) {
        SearchResult result;
        result.nodes = nodes_;
        return result;
    }

    SearchResult best_completed;
    best_completed.best_move = moves.front();
    best_completed.principal_variation.push_back(moves.front());
    const int max_depth = std::max(1, limits.max_depth);

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (should_stop(true)) {
            break;
        }

        SearchResult iteration = search_depth(board, depth);
        iteration.nodes = nodes_;

        if (stop_ && depth > 1) {
            break;
        }

        if (iteration.best_move) {
            best_completed = iteration;
            if (limits.on_depth_finished) {
                limits.on_depth_finished(best_completed);
            }
        }

        if (should_stop(true)) {
            break;
        }
    }

    best_completed.nodes = nodes_;
    return best_completed;
}

SearchResult Search::search_depth(Board& board, int depth)
{
    SearchResult result;
    result.depth = depth;

    auto moves = ordered_moves(board);
    if (moves.empty()) {
        result.nodes = nodes_;
        return result;
    }

    int best_score = -infinity;
    int alpha = -infinity;
    const int beta = infinity;

    for (const Move& move : moves) {
        if (should_stop(true)) {
            break;
        }

        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }

        const int score = -negamax(board, depth - 1, 1, -beta, -alpha);
        board.unmake_move(undo);

        if (!result.best_move || score > best_score) {
            best_score = score;
            result.best_move = move;
            result.score = score;
        }

        alpha = std::max(alpha, score);
    }

    result.nodes = nodes_;
    if (result.best_move) {
        result.principal_variation.push_back(*result.best_move);
    }
    return result;
}

int Search::negamax(Board& board, int depth, int ply, int alpha, int beta)
{
    ++nodes_;
    if (should_stop()) {
        return 0;
    }

    if (depth <= 0) {
        return quiescence(board, ply, alpha, beta);
    }

    const auto moves = ordered_moves(board);
    if (moves.empty()) {
        if (board.in_check(board.side_to_move())) {
            return -mate_score + ply;
        }
        return 0;
    }

    int best = -infinity;
    for (const Move& move : moves) {
        if (should_stop()) {
            break;
        }

        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }

        const int score = -negamax(board, depth - 1, ply + 1, -beta, -alpha);
        board.unmake_move(undo);

        best = std::max(best, score);
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            break;
        }
    }

    return best;
}

int Search::quiescence(Board& board, int ply, int alpha, int beta)
{
    ++nodes_;
    if (should_stop()) {
        return 0;
    }

    const int stand_pat = evaluate(board);
    if (stand_pat >= beta) {
        return beta;
    }
    alpha = std::max(alpha, stand_pat);

    for (const Move& move : ordered_moves(board)) {
        if (should_stop()) {
            break;
        }

        if (!is_tactical(move)) {
            continue;
        }

        Board::UndoState undo;
        if (!board.make_move(move, undo)) {
            continue;
        }

        const int score = -quiescence(board, ply + 1, -beta, -alpha);
        board.unmake_move(undo);

        if (score >= beta) {
            return beta;
        }
        alpha = std::max(alpha, score);
    }

    (void)ply;
    return alpha;
}

int Search::evaluate(const Board& board) const
{
    int score = 0;

    for (int type = 0; type < 6; ++type) {
        const PieceType piece_type = static_cast<PieceType>(type);
        const int value = piece_values[type];
        score += value * static_cast<int>(std::popcount(board.pieces(Color::White, piece_type)));
        score -= value * static_cast<int>(std::popcount(board.pieces(Color::Black, piece_type)));
    }

    return board.side_to_move() == Color::White ? score : -score;
}

std::vector<Move> Search::ordered_moves(const Board& board) const
{
    auto moves = board.generate_legal_moves();
    std::stable_sort(moves.begin(), moves.end(), [](const Move& left, const Move& right) {
        return is_tactical(left) && !is_tactical(right);
    });
    return moves;
}

bool Search::should_stop(bool force_check)
{
    if (stop_) {
        return true;
    }

    if (!deadline_) {
        return false;
    }

    if (!force_check && (nodes_ & 2047ULL) != 0) {
        return false;
    }

    stop_ = std::chrono::steady_clock::now() >= *deadline_;
    return stop_;
}

std::string pv_to_string(const std::vector<Move>& moves)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < moves.size(); ++index) {
        if (index != 0) {
            output << ' ';
        }
        output << move_to_uci(moves[index]);
    }
    return output.str();
}

} // namespace aetherz
