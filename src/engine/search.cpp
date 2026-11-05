#include "engine/search.hpp"

#include "engine/evaluate.hpp"
#include "engine/see.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <limits>
#include <sstream>

namespace aetherz {
namespace {

constexpr int infinity = 32000;
constexpr int initial_aspiration_window = 35;

std::int64_t deadline_after(std::chrono::milliseconds duration)
{
    return (std::chrono::steady_clock::now() + duration).time_since_epoch().count();
}

bool is_tactical(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::Promotion
        || move.flag == MoveFlag::PromotionCapture;
}

bool is_capture_like(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::PromotionCapture;
}

bool is_quiet(const Move& move)
{
    return !is_tactical(move)
        && move.flag != MoveFlag::KingCastle
        && move.flag != MoveFlag::QueenCastle;
}

bool same_move(const Move& left, const Move& right)
{
    return left.from == right.from
        && left.to == right.to
        && left.promotion == right.promotion;
}

int piece_value(PieceType type)
{
    return middlegame_piece_value(type);
}

int move_key(const Move& move)
{
    return (move.from << 9) | (move.to << 3) | static_cast<int>(move.promotion);
}

int capture_score(const Board& board, const Move& move)
{
    const Piece attacker = board.piece_at(move.from);
    Piece victim = board.piece_at(move.to);

    if (move.flag == MoveFlag::EnPassant) {
        victim = make_piece(opposite(board.side_to_move()), PieceType::Pawn);
    }

    return 10 * piece_value(piece_type(victim)) - piece_value(piece_type(attacker));
}

int promotion_score(PieceType type)
{
    return piece_value(type);
}

int reverse_futility_margin(int depth)
{
    return 90 * depth;
}

int futility_margin(int depth)
{
    return 140 * depth;
}

int razoring_margin(int depth)
{
    return 180 + 120 * depth;
}

int probcut_margin(int depth)
{
    return 150 + 20 * depth;
}

bool make_legal_move(Board& board, const Move& move, Board::UndoState& undo)
{
    const Color us = board.side_to_move();
    if (!board.make_move(move, undo)) {
        return false;
    }

    if (board.in_check(us)) {
        board.unmake_move(undo);
        return false;
    }

    return true;
}

int score_to_table(int score, int ply)
{
    if (score > mate_score - 1000) {
        return score + ply;
    }
    if (score < -mate_score + 1000) {
        return score - ply;
    }
    return score;
}

int score_from_table(int score, int ply)
{
    if (score > mate_score - 1000) {
        return score - ply;
    }
    if (score < -mate_score + 1000) {
        return score + ply;
    }
    return score;
}

} // namespace

TranspositionTable::TranspositionTable(std::size_t megabytes)
{
    resize(megabytes);
}

void TranspositionTable::resize(std::size_t megabytes)
{
    const std::size_t bytes = std::max<std::size_t>(1, megabytes) * 1024ULL * 1024ULL;
    const std::size_t cluster_count = std::max<std::size_t>(1, bytes / sizeof(Cluster));
    clusters_.assign(cluster_count, Cluster{});
}

void TranspositionTable::clear()
{
    std::fill(clusters_.begin(), clusters_.end(), Cluster{});
    generation_ = 1;
}

void TranspositionTable::new_search()
{
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
    }
}

std::size_t TranspositionTable::size_mb() const
{
    return clusters_.size() * sizeof(Cluster) / (1024ULL * 1024ULL);
}

int TranspositionTable::hashfull() const
{
    const std::size_t sample = std::min<std::size_t>(clusters_.size(), 1000 / cluster_size);
    int used = 0;
    for (std::size_t index = 0; index < sample; ++index) {
        for (const TranspositionEntry& entry : clusters_[index].entries) {
            used += entry.depth >= 0 && entry.generation == generation_ ? 1 : 0;
        }
    }
    return sample == 0 ? 0 : static_cast<int>(used * 1000 / (sample * cluster_size));
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key) const
{
    if (clusters_.empty()) {
        return std::nullopt;
    }

    const Cluster& cluster = clusters_[key % clusters_.size()];
    for (const TranspositionEntry& entry : cluster.entries) {
        if (entry.depth >= 0 && entry.key == key) {
            return entry;
        }
    }

    return std::nullopt;
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, Bound bound, Move best_move)
{
    if (clusters_.empty()) {
        return;
    }

    Cluster& cluster = clusters_[key % clusters_.size()];
    auto replacement_value = [this](const TranspositionEntry& entry) {
        if (entry.depth < 0) {
            return std::numeric_limits<int>::min();
        }

        const int age = static_cast<std::uint8_t>(generation_ - entry.generation);
        const int exact_bonus = entry.bound == Bound::Exact ? 3 : 0;
        return entry.depth * 4 + exact_bonus - age * 2;
    };

    TranspositionEntry* target = &cluster.entries[0];
    for (TranspositionEntry& entry : cluster.entries) {
        if (entry.key == key && entry.depth >= 0) {
            target = &entry;
            break;
        }
        if (replacement_value(entry) < replacement_value(*target)) {
            target = &entry;
        }
    }

    if (target->depth >= 0 && target->key == key && target->depth > depth && target->bound == Bound::Exact) {
        return;
    }

    *target = TranspositionEntry{
        .key = key,
        .best_move = best_move,
        .score = score,
        .depth = depth,
        .generation = generation_,
        .bound = bound,
    };
}

Search::Search()
    : table_(32)
{
}

SearchResult Search::find_best_move(Board& board, int depth)
{
    SearchLimits limits;
    limits.max_depth = depth;
    return find_best_move(board, limits);
}

SearchResult Search::find_best_move(Board& board, const SearchLimits& limits)
{
    nodes_ = 0;
    tt_hits_ = 0;
    stop_ = false;
    max_nodes_ = limits.max_nodes;
    set_time_limits(limits.soft_time, limits.move_time);
    table_.new_search();
    clear_killers();
    reset_position_history(board, limits);
    refresh_nnue(board);
    if (limits.on_search_started) {
        limits.on_search_started();
    }

    SearchResult best_completed;
    const int max_depth = std::clamp(limits.max_depth, 1, max_search_depth);
    int previous_score = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (should_stop(true)) {
            break;
        }

        int alpha = -infinity;
        int beta = infinity;
        if (depth > 1) {
            alpha = previous_score - initial_aspiration_window;
            beta = previous_score + initial_aspiration_window;
        }

        SearchResult iteration;
        int window = initial_aspiration_window;
        seldepth_ = 0;
        while (true) {
            iteration = search_depth(board, depth, alpha, beta);
            iteration.seldepth = std::max(seldepth_, depth);
            iteration.nodes = nodes_;
            iteration.tt_hits = tt_hits_;

            if (stop_ || depth == 1 || (iteration.score > alpha && iteration.score < beta)) {
                break;
            }

            // Re-search around the score that fell outside the window, and give
            // up on windows once they get wide.
            window *= 2;
            if (iteration.score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-infinity, iteration.score - window);
            } else {
                beta = std::min(infinity, iteration.score + window);
            }
            if (window > 1000) {
                alpha = -infinity;
                beta = infinity;
            }
        }

        if (stop_ && depth > 1) {
            break;
        }

        if (iteration.best_move || (!stop_ && !best_completed.best_move)) {
            previous_score = iteration.score;
            best_completed = iteration;
            if (limits.on_depth_finished) {
                limits.on_depth_finished(best_completed);
            }
        }

        if (should_stop(true) || soft_limit_reached()) {
            break;
        }
    }

    best_completed.nodes = nodes_;
    best_completed.tt_hits = tt_hits_;
    return best_completed;
}

void Search::request_stop()
{
    stop_ = true;
}

void Search::set_time_limits(
    std::optional<std::chrono::milliseconds> soft_time,
    std::optional<std::chrono::milliseconds> hard_time)
{
    soft_deadline_ = soft_time ? deadline_after(*soft_time) : 0;
    hard_deadline_ = hard_time ? deadline_after(*hard_time) : 0;
}

void Search::set_hash_size_mb(std::size_t megabytes)
{
    table_.resize(megabytes);
}

void Search::clear_hash()
{
    table_.clear();
}

void Search::clear()
{
    table_.clear();
    clear_killers();
    for (auto& row : history_) {
        row.fill(0);
    }
    for (auto& table : continuation_history_) {
        for (auto& row : table) {
            row.fill(0);
        }
    }
    correction_history_.fill(0);
}

int Search::hashfull() const
{
    return table_.hashfull();
}

std::size_t Search::hash_size_mb() const
{
    return table_.size_mb();
}

void Search::set_use_nnue(bool enabled)
{
    use_nnue_ = enabled;
}

bool Search::use_nnue() const
{
    return use_nnue_;
}

bool Search::load_nnue(std::string_view path, std::string* error)
{
    return nnue_.load(path, error);
}

void Search::clear_nnue()
{
    nnue_.clear();
}

bool Search::nnue_loaded() const
{
    return nnue_.loaded();
}

bool Search::nnue_ready() const
{
    return nnue_.ready();
}

std::string Search::nnue_eval_file() const
{
    return nnue_.eval_file();
}

SearchResult Search::search_depth(Board& board, int depth, int alpha, int beta)
{
    SearchResult result;
    result.depth = depth;

    const bool in_check = board.in_check(board.side_to_move());
    const auto entry = table_.probe(board.hash());
    MovePicker picker = ordered_moves(board, entry ? std::optional<Move>{entry->best_move} : std::nullopt, 0);
    MoveList moves;
    while (const auto move = picker.next()) {
        moves.push_back(*move);
    }
    if (moves.empty()) {
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
        result.score = in_check ? -mate_score : 0;
        return result;
    }

    if (is_draw(board, 0)) {
        for (const Move& move : moves) {
            Board::UndoState undo;
            if (!make_legal_move(board, move, undo)) {
                continue;
            }
            board.unmake_move(undo);
            result.best_move = move;
            result.principal_variation.push_back(move);
            break;
        }
        result.score = 0;
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
        return result;
    }

    int best_score = -infinity;
    const int original_alpha = alpha;
    const int original_beta = beta;
    bool searched_move = false;

    for (std::size_t index = 0; index < moves.size(); ++index) {
        if (should_stop(true)) {
            break;
        }

        const Move& move = moves[index];
        const int see_gain = is_capture_like(move) ? static_exchange_evaluation(board, move) : 0;
        Board::UndoState undo;
        if (!make_legal_move(board, move, undo)) {
            continue;
        }
        const bool gives_check = board.in_check(board.side_to_move());
        push_position(board);
        push_nnue(board, undo);

        const int extension = tactical_extension(move, depth, 0, gives_check, see_gain);
        const int child_depth = std::max(0, depth - 1 + extension);
        int score = 0;
        if (!searched_move) {
            score = -negamax(board, child_depth, 1, -beta, -alpha, move);
        } else {
            score = -negamax(board, child_depth, 1, -alpha - 1, -alpha, move);
            if (!stop_ && score > alpha && score < beta) {
                score = -negamax(board, child_depth, 1, -beta, -alpha, move);
            }
        }
        pop_nnue();
        pop_position();
        board.unmake_move(undo);
        searched_move = true;

        if (!result.best_move || score > best_score) {
            best_score = score;
            result.best_move = move;
            result.score = score;
        }

        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            record_cutoff(move, depth, 0, {});
            break;
        }
    }

    if (!searched_move) {
        result.score = in_check ? -mate_score : 0;
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
        return result;
    }

    result.nodes = nodes_;
    result.tt_hits = tt_hits_;
    if (result.best_move) {
        Bound bound = Bound::Exact;
        if (best_score <= original_alpha) {
            bound = Bound::Upper;
        } else if (best_score >= original_beta) {
            bound = Bound::Lower;
        }

        table_.store(board.hash(), depth, score_to_table(result.score, 0), bound, *result.best_move);
        if (bound == Bound::Exact && !in_check) {
            update_correction_history(board, depth, result.score);
        }
        result.principal_variation = principal_variation(board, depth);
    }
    return result;
}

std::vector<Move> Search::principal_variation(Board board, int depth) const
{
    std::vector<Move> line;
    line.reserve(static_cast<std::size_t>(std::max(0, depth)));

    for (int remaining = depth; remaining > 0; --remaining) {
        const auto entry = table_.probe(board.hash());
        if (!entry || !entry->best_move.is_valid()) {
            break;
        }

        bool made_move = false;
        for (const Move& legal_move : board.generate_pseudo_legal_moves()) {
            if (!same_move(legal_move, entry->best_move)) {
                continue;
            }

            Board::UndoState undo;
            if (!make_legal_move(board, legal_move, undo)) {
                return line;
            }
            line.push_back(legal_move);
            made_move = true;
            break;
        }

        if (!made_move) {
            break;
        }
    }

    return line;
}

int Search::negamax(Board& board, int depth, int ply, int alpha, int beta, Move previous_move)
{
    ++nodes_;
    seldepth_ = std::max(seldepth_, ply);
    if (should_stop()) {
        return 0;
    }
    if (is_draw(board, ply)) {
        return 0;
    }
    if (ply >= max_ply - 1) {
        return evaluate(board);
    }

    if (depth <= 0) {
        return quiescence(board, ply, alpha, beta, 1);
    }

    // Mate distance pruning: no line from here can beat a mate already found
    // closer to the root.
    alpha = std::max(alpha, -mate_score + ply);
    beta = std::min(beta, mate_score - ply - 1);
    if (alpha >= beta) {
        return alpha;
    }

    const int original_alpha = alpha;
    const int original_beta = beta;
    const bool in_check = board.in_check(board.side_to_move());
    std::optional<int> static_eval;
    const auto current_static_eval = [&]() {
        if (!static_eval) {
            static_eval = evaluate(board);
        }
        return *static_eval;
    };

    std::optional<Move> tt_move;
    if (const auto entry = table_.probe(board.hash())) {
        tt_move = entry->best_move;
        if (entry->depth >= depth) {
            ++tt_hits_;
            const int table_score = score_from_table(entry->score, ply);
            if (entry->bound == Bound::Exact) {
                return table_score;
            }
            if (entry->bound == Bound::Lower) {
                alpha = std::max(alpha, table_score);
            } else if (entry->bound == Bound::Upper) {
                beta = std::min(beta, table_score);
            }
            if (alpha >= beta) {
                return table_score;
            }
        }
    }

    if (!tt_move && depth >= 5 && ply > 0 && !in_check && beta == alpha + 1) {
        --depth;
    }

    if (can_try_razoring(depth, ply, alpha, beta, in_check, current_static_eval())) {
        const int score = quiescence(board, ply, alpha, beta, 0);
        if (score <= alpha) {
            return score;
        }
    }

    if (!in_check
        && depth <= 3
        && beta < mate_score - 1000
        && beta > -mate_score + 1000
        && can_prune_reverse_futility(depth, ply, beta, in_check, current_static_eval())) {
        return current_static_eval() - reverse_futility_margin(depth);
    }

    if (can_try_probcut(depth, ply, beta, in_check, current_static_eval())) {
        const int probcut_beta = beta + probcut_margin(depth);
        const int probcut_depth = std::max(0, depth - 4);
        MovePicker probcut_moves = ordered_moves(board, tt_move, ply, previous_move, MoveGeneration::Tactical);
        while (const auto next_move = probcut_moves.next()) {
            const Move& move = *next_move;
            if (is_capture_like(move) && static_exchange_evaluation(board, move) < 0) {
                continue;
            }

            Board::UndoState undo;
            if (!make_legal_move(board, move, undo)) {
                continue;
            }
            push_position(board);
            push_nnue(board, undo);
            const int score = -negamax(board, probcut_depth, ply + 1, -probcut_beta, -probcut_beta + 1, move);
            pop_nnue();
            pop_position();
            board.unmake_move(undo);

            if (score >= probcut_beta) {
                return score;
            }
        }
    }

    if (can_try_null_move(board, depth, ply, alpha, beta)) {
        Board::UndoState undo;
        if (board.make_null_move(undo)) {
            push_nnue_null(board);
            const int reduction = 2 + depth / 4;
            const int score = -negamax(board, depth - 1 - reduction, ply + 1, -beta, -beta + 1, {});
            pop_nnue();
            board.unmake_move(undo);

            if (score >= beta) {
                // Do not trust mate scores from a null-move search.
                return score >= mate_bound ? beta : score;
            }
        }
    }

    MovePicker moves = ordered_moves(board, tt_move, ply, previous_move);
    if (moves.empty()) {
        if (in_check) {
            return -mate_score + ply;
        }
        return 0;
    }

    int best = -infinity;
    Move best_move;
    bool searched_move = false;
    int searched_moves = 0;
    while (const auto next_move = moves.next()) {
        const Move& move = *next_move;
        if (should_stop()) {
            break;
        }

        Board::UndoState undo;
        const int see_gain = is_capture_like(move) ? static_exchange_evaluation(board, move) : 0;
        if (!make_legal_move(board, move, undo)) {
            continue;
        }
        const bool gives_check = board.in_check(board.side_to_move());
        const int move_index = searched_moves + 1;
        if (depth <= 2
            && move_index > 1
            && !in_check
            && !gives_check
            && is_quiet(move)
            && alpha > -mate_score + 1000
            && can_prune_futility(move, depth, move_index, alpha, in_check, gives_check, current_static_eval())) {
            board.unmake_move(undo);
            continue;
        }
        if (can_prune_late_move(move, depth, move_index, ply, in_check, gives_check, previous_move)) {
            board.unmake_move(undo);
            continue;
        }

        push_position(board);
        push_nnue(board, undo);

        ++searched_moves;
        const int extension = tactical_extension(move, depth, ply, gives_check, see_gain);
        const int full_depth = std::max(0, depth - 1 + extension);
        const int reduction = searched_move
            ? late_move_reduction(move, depth, ply, searched_moves, in_check, gives_check, previous_move)
            : 0;
        const int child_depth = std::max(0, full_depth - reduction);

        int score = 0;
        if (!searched_move) {
            score = -negamax(board, child_depth, ply + 1, -beta, -alpha, move);
        } else {
            score = -negamax(board, child_depth, ply + 1, -alpha - 1, -alpha, move);
            if (reduction > 0 && !stop_ && score > alpha) {
                score = -negamax(board, full_depth, ply + 1, -alpha - 1, -alpha, move);
            }
            if (!stop_ && score > alpha && score < beta) {
                score = -negamax(board, full_depth, ply + 1, -beta, -alpha, move);
            }
        }
        pop_nnue();
        pop_position();
        board.unmake_move(undo);
        searched_move = true;

        if (score > best) {
            best = score;
            best_move = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            record_cutoff(move, depth, ply, previous_move);
            break;
        }
    }

    if (!searched_move) {
        if (in_check) {
            return -mate_score + ply;
        }
        return 0;
    }

    if (!stop_ && best_move.is_valid()) {
        Bound bound = Bound::Exact;
        if (best <= original_alpha) {
            bound = Bound::Upper;
        } else if (best >= original_beta) {
            bound = Bound::Lower;
        }

        table_.store(board.hash(), depth, score_to_table(best, ply), bound, best_move);
        if (bound == Bound::Exact && !in_check) {
            update_correction_history(board, depth, best);
        }
    }

    return best;
}

int Search::quiescence(Board& board, int ply, int alpha, int beta, int quiet_check_depth)
{
    ++nodes_;
    seldepth_ = std::max(seldepth_, ply);
    if (should_stop()) {
        return 0;
    }
    if (is_draw(board, ply)) {
        return 0;
    }
    if (ply >= max_ply - 1) {
        return evaluate(board);
    }

    const bool in_check = board.in_check(board.side_to_move());
    int best = -infinity;
    if (!in_check) {
        const int stand_pat = evaluate(board);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        best = stand_pat;
        alpha = std::max(alpha, stand_pat);
    }

    bool searched_move = false;
    const bool include_quiet_checks = !in_check && quiet_check_depth > 0;
    const MoveGeneration generation = in_check || include_quiet_checks ? MoveGeneration::All : MoveGeneration::Tactical;
    MovePicker moves = ordered_moves(board, std::nullopt, ply, {}, generation);
    while (const auto next_move = moves.next()) {
        const Move& move = *next_move;
        if (should_stop()) {
            break;
        }

        if (!in_check && is_capture_like(move) && static_exchange_evaluation(board, move) < 0) {
            continue;
        }

        Board::UndoState undo;
        if (!make_legal_move(board, move, undo)) {
            continue;
        }
        const bool gives_check = board.in_check(board.side_to_move());
        if (!in_check && !is_tactical(move) && !gives_check) {
            board.unmake_move(undo);
            continue;
        }
        push_position(board);
        push_nnue(board, undo);
        searched_move = true;

        const int next_quiet_check_depth = !in_check && !is_tactical(move) ? quiet_check_depth - 1 : 0;
        const int score = -quiescence(board, ply + 1, -beta, -alpha, next_quiet_check_depth);
        pop_nnue();
        pop_position();
        board.unmake_move(undo);

        if (score >= beta) {
            return score;
        }
        best = std::max(best, score);
        alpha = std::max(alpha, score);
    }

    if (in_check && !searched_move && !stop_) {
        return -mate_score + ply;
    }

    return best;
}

int Search::evaluate(const Board& board) const
{
    return std::clamp(raw_evaluate(board) + correction(board), -mate_score + 1000, mate_score - 1000);
}

int Search::raw_evaluate(const Board& board) const
{
    if (can_use_nnue()) {
        return nnue_.evaluate(board);
    }

    return evaluate_side_to_move(board);
}

int Search::correction(const Board& board) const
{
    return correction_history_[board.hash() % correction_history_.size()];
}

std::optional<Move> MovePicker::next()
{
    if (index_ >= count_) {
        return std::nullopt;
    }

    // Stage first, then score, then the move itself so the order does not
    // depend on the order in which moves were generated.
    auto better = [](const ScoredMove& left, const ScoredMove& right) {
        if (left.stage != right.stage) {
            return left.stage < right.stage;
        }
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.key < right.key;
    };

    // The first move often causes a cutoff, so only select it; sort the rest
    // once a second move is actually needed.
    if (index_ == 0) {
        std::size_t best = 0;
        for (std::size_t index = 1; index < count_; ++index) {
            if (better(moves_[index], moves_[best])) {
                best = index;
            }
        }
        std::swap(moves_[0], moves_[best]);
    } else if (index_ == 1) {
        std::sort(moves_.begin() + 1, moves_.begin() + static_cast<std::ptrdiff_t>(count_), better);
    }
    return moves_[index_++].move;
}

MovePicker Search::ordered_moves(
    const Board& board,
    std::optional<Move> tt_move,
    int ply,
    Move previous_move,
    MoveGeneration generation) const
{
    const MoveList moves = generation == MoveGeneration::Tactical
        ? board.generate_pseudo_legal_tactical_moves()
        : board.generate_pseudo_legal_moves();

    enum Stage : int {
        TtMove,
        Promotions,
        GoodCaptures,
        PriorityQuiets,
        Quiets,
        BadCaptures,
    };

    MovePicker picker;
    for (const Move& move : moves) {
        MovePicker::ScoredMove entry{.move = move, .stage = Quiets, .score = 0, .key = move_key(move)};

        if (tt_move && same_move(move, *tt_move)) {
            entry.stage = TtMove;
        } else if (move.flag == MoveFlag::Promotion || move.flag == MoveFlag::PromotionCapture) {
            entry.stage = Promotions;
            entry.score = promotion_score(move.promotion);
            if (is_capture_like(move)) {
                entry.score += capture_score(board, move);
            }
        } else if (is_capture_like(move)) {
            const int see = static_exchange_evaluation(board, move);
            entry.stage = see >= 0 ? GoodCaptures : BadCaptures;
            entry.score = see * 8 + capture_score(board, move);
        } else {
            int score = 0;
            if (move.flag == MoveFlag::KingCastle || move.flag == MoveFlag::QueenCastle) {
                score += 750'000;
            }

            if (ply >= 0 && ply < max_ply) {
                if (same_move(move, killer_moves_[ply][0])) {
                    score += 700'000;
                } else if (same_move(move, killer_moves_[ply][1])) {
                    score += 650'000;
                }
            }

            score += quiet_history_score(move, previous_move);
            entry.stage = score >= 650'000 ? PriorityQuiets : Quiets;
            entry.score = score;
        }

        picker.add(entry);
    }

    return picker;
}

bool Search::should_stop(bool force_check)
{
    if (stop_.load(std::memory_order_relaxed)) {
        return true;
    }

    if (max_nodes_ && nodes_ >= *max_nodes_) {
        stop_ = true;
        return true;
    }

    if (!force_check && (nodes_ & 2047ULL) != 0) {
        return false;
    }

    const std::int64_t deadline = hard_deadline_.load(std::memory_order_relaxed);
    if (deadline != 0 && std::chrono::steady_clock::now().time_since_epoch().count() >= deadline) {
        stop_ = true;
    }
    return stop_.load(std::memory_order_relaxed);
}

bool Search::soft_limit_reached() const
{
    const std::int64_t deadline = soft_deadline_.load(std::memory_order_relaxed);
    return deadline != 0 && std::chrono::steady_clock::now().time_since_epoch().count() >= deadline;
}

bool Search::can_try_razoring(int depth, int ply, int alpha, int beta, bool in_check, int static_eval) const
{
    if (depth > 2 || ply <= 0 || in_check) {
        return false;
    }
    if (beta != alpha + 1) {
        return false;
    }
    if (alpha <= -mate_score + 1000 || beta >= mate_score - 1000) {
        return false;
    }
    return static_eval + razoring_margin(depth) <= alpha;
}

bool Search::can_try_probcut(int depth, int ply, int beta, bool in_check, int static_eval) const
{
    if (depth < 4 || ply <= 0 || in_check) {
        return false;
    }
    if (beta >= mate_score - 1000 || beta <= -mate_score + 1000) {
        return false;
    }
    return static_eval + probcut_margin(depth) >= beta;
}

bool Search::can_try_null_move(const Board& board, int depth, int ply, int alpha, int beta) const
{
    if (depth < 3 || ply <= 0) {
        return false;
    }
    if (beta != alpha + 1) {
        return false;
    }
    if (board.in_check(board.side_to_move())) {
        return false;
    }
    if (beta >= mate_score - 1000 || beta <= -mate_score + 1000) {
        return false;
    }
    if (board.halfmove_clock() >= 99) {
        return false;
    }
    return has_non_pawn_material(board, board.side_to_move());
}

bool Search::can_prune_reverse_futility(int depth, int ply, int beta, bool in_check, int static_eval) const
{
    if (depth > 3 || ply <= 0 || in_check) {
        return false;
    }
    if (beta >= mate_score - 1000 || beta <= -mate_score + 1000) {
        return false;
    }
    return static_eval - reverse_futility_margin(depth) >= beta;
}

bool Search::can_prune_futility(
    const Move& move,
    int depth,
    int move_index,
    int alpha,
    bool in_check,
    bool gives_check,
    int static_eval) const
{
    if (depth > 2 || move_index <= 1 || in_check || gives_check) {
        return false;
    }
    if (!is_quiet(move) || alpha <= -mate_score + 1000) {
        return false;
    }
    return static_eval + futility_margin(depth) <= alpha;
}

bool Search::can_prune_late_move(
    const Move& move,
    int depth,
    int move_index,
    int ply,
    bool in_check,
    bool gives_check,
    Move previous_move) const
{
    if (depth > 2 || in_check || gives_check || !is_quiet(move)) {
        return false;
    }
    if (ply >= 0 && ply < max_ply && (same_move(move, killer_moves_[ply][0]) || same_move(move, killer_moves_[ply][1]))) {
        return false;
    }
    if (quiet_history_score(move, previous_move) > 0) {
        return false;
    }

    const int threshold = depth == 1 ? 12 : 24;
    return move_index > threshold;
}

bool Search::has_non_pawn_material(const Board& board, Color color) const
{
    return (board.pieces(color, PieceType::Knight)
        | board.pieces(color, PieceType::Bishop)
        | board.pieces(color, PieceType::Rook)
        | board.pieces(color, PieceType::Queen))
        != 0;
}

int Search::tactical_extension(const Move& move, int depth, int ply, bool gives_check, int see_gain) const
{
    if (ply >= max_ply - 1) {
        return 0;
    }

    if (gives_check && depth <= 4 && move.flag != MoveFlag::KingCastle && move.flag != MoveFlag::QueenCastle) {
        return 1;
    }
    if (depth <= 3 && (move.flag == MoveFlag::Promotion || move.flag == MoveFlag::PromotionCapture)) {
        return 1;
    }
    if (depth <= 1 && is_capture_like(move) && see_gain >= middlegame_piece_value(PieceType::Queen)) {
        return 1;
    }

    return 0;
}

int Search::late_move_reduction(
    const Move& move,
    int depth,
    int ply,
    int move_index,
    bool in_check,
    bool gives_check,
    Move previous_move) const
{
    if (depth < 3 || ply <= 0 || in_check || gives_check || move_index < 4) {
        return 0;
    }
    if (!is_quiet(move)) {
        return 0;
    }
    if (ply < max_ply && (same_move(move, killer_moves_[ply][0]) || same_move(move, killer_moves_[ply][1]))) {
        return 0;
    }

    int reduction = 1;
    if (depth >= 5 && move_index >= 6) {
        ++reduction;
    }
    if (depth >= 7 && move_index >= 12) {
        ++reduction;
    }
    if (depth >= 6 && move_index >= 20) {
        ++reduction;
    }

    const int history_score = quiet_history_score(move, previous_move);
    if (history_score > 0) {
        --reduction;
    }
    if (history_score > 400) {
        --reduction;
    }

    return std::clamp(reduction, 0, depth - 1);
}

bool Search::is_draw(const Board& board, int ply) const
{
    // Inside the tree a single repetition is scored as a draw: if repeating
    // is good for one side it can repeat again. The root needs a real
    // threefold repetition.
    return board.halfmove_clock() >= 100 || repetition_count(board) >= (ply > 0 ? 2 : 3);
}

int Search::repetition_count(const Board& board) const
{
    // Positions before the last capture or pawn move cannot repeat.
    const auto window = std::min<std::size_t>(position_history_.size(), static_cast<std::size_t>(board.halfmove_clock()) + 1);
    return static_cast<int>(std::count(position_history_.end() - static_cast<std::ptrdiff_t>(window), position_history_.end(), board.hash()));
}

int Search::continuation_score(const Move& move, Move previous_move) const
{
    if (!previous_move.is_valid() || !is_valid_square(previous_move.to) || !is_valid_square(move.from) || !is_valid_square(move.to)) {
        return 0;
    }

    return continuation_history_[previous_move.to][move.from][move.to] / 2;
}

int Search::quiet_history_score(const Move& move, Move previous_move) const
{
    if (!is_valid_square(move.from) || !is_valid_square(move.to)) {
        return 0;
    }

    return history_[move.from][move.to] + continuation_score(move, previous_move);
}

void Search::reset_position_history(const Board& board, const SearchLimits& limits)
{
    position_history_ = limits.position_history;
    if (position_history_.empty() || position_history_.back() != board.hash()) {
        position_history_.push_back(board.hash());
    }
}

void Search::push_position(const Board& board)
{
    position_history_.push_back(board.hash());
}

void Search::pop_position()
{
    if (!position_history_.empty()) {
        position_history_.pop_back();
    }
}

void Search::refresh_nnue(const Board& board)
{
    if (can_use_nnue()) {
        nnue_.refresh(board);
    }
}

void Search::push_nnue(const Board& board, const Board::UndoState& undo)
{
    if (can_use_nnue()) {
        nnue_.push(board, nnue::dirty_move_from(board, undo));
    }
}

void Search::push_nnue_null(const Board& board)
{
    if (can_use_nnue()) {
        nnue_.push(board, nnue::DirtyMove{});
    }
}

void Search::pop_nnue()
{
    if (can_use_nnue()) {
        nnue_.pop();
    }
}

bool Search::can_use_nnue() const
{
    return use_nnue_ && nnue_.ready();
}

void Search::clear_killers()
{
    for (auto& ply_killers : killer_moves_) {
        ply_killers = {};
    }
}

void Search::record_cutoff(const Move& move, int depth, int ply, Move previous_move)
{
    if (is_tactical(move)) {
        return;
    }

    if (ply >= 0 && ply < max_ply && !same_move(move, killer_moves_[ply][0])) {
        killer_moves_[ply][1] = killer_moves_[ply][0];
        killer_moves_[ply][0] = move;
    }

    if (move.from >= 0 && move.to >= 0) {
        const int bonus = depth * depth;
        history_[move.from][move.to] = std::clamp(history_[move.from][move.to] + bonus, -16'000, 16'000);
        if (previous_move.is_valid() && is_valid_square(previous_move.to)) {
            auto& continuation = continuation_history_[previous_move.to][move.from][move.to];
            continuation = std::clamp(continuation + bonus * 2, -16'000, 16'000);
        }
    }
}

void Search::update_correction_history(const Board& board, int depth, int search_score)
{
    if (depth < 2 || search_score >= mate_score - 1000 || search_score <= -mate_score + 1000) {
        return;
    }

    const int target = std::clamp(search_score - raw_evaluate(board), -256, 256);
    int& entry = correction_history_[board.hash() % correction_history_.size()];
    const int weight = std::clamp(depth, 1, 8);
    const int delta = target - entry;
    entry += delta * weight / 32;
    if (entry != target && std::abs(delta) < 32) {
        entry += delta > 0 ? 1 : -1;
    }
    entry = std::clamp(entry, -256, 256);
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
