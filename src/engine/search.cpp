#include "engine/search.hpp"

#include "engine/evaluate.hpp"
#include "engine/see.hpp"
#include "engine/thread.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
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

// Base late-move reductions by depth and move number.
const std::array<std::array<int, 64>, 64> lmr_table = [] {
    std::array<std::array<int, 64>, 64> table{};
    for (int depth = 1; depth < 64; ++depth) {
        for (int move_number = 1; move_number < 64; ++move_number) {
            table[depth][move_number] = static_cast<int>(0.75 + std::log(depth) * std::log(move_number) / 2.25);
        }
    }
    return table;
}();

int reverse_futility_margin(int depth, bool improving)
{
    return 80 * (depth - (improving ? 1 : 0));
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
    generation_ = static_cast<std::uint8_t>((generation_ + 1) & 0x3f);
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
        for (const PackedEntry& entry : clusters_[index].entries) {
            used += !entry.empty() && entry.generation() == generation_ ? 1 : 0;
        }
    }
    return sample == 0 ? 0 : static_cast<int>(used * 1000 / (sample * cluster_size));
}

TranspositionTable::Cluster& TranspositionTable::cluster_for(std::uint64_t key)
{
    return clusters_[key % clusters_.size()];
}

const TranspositionTable::Cluster& TranspositionTable::cluster_for(std::uint64_t key) const
{
    return clusters_[key % clusters_.size()];
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key) const
{
    if (clusters_.empty()) {
        return std::nullopt;
    }

    const std::uint32_t verification = verification_key(key);
    for (const PackedEntry& entry : cluster_for(key).entries) {
        if (entry.empty() || entry.key != verification) {
            continue;
        }

        Move move;
        if (entry.move != 0) {
            move.from = entry.move & 0x3f;
            move.to = (entry.move >> 6) & 0x3f;
            move.promotion = static_cast<PieceType>((entry.move >> 12) & 0x7);
        }
        return TranspositionEntry{
            .best_move = move,
            .score = entry.score,
            .eval = entry.eval,
            .depth = entry.depth - depth_offset,
            .bound = static_cast<Bound>(entry.generation_bound & 0x3),
        };
    }

    return std::nullopt;
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, int eval, Bound bound, Move best_move)
{
    if (clusters_.empty()) {
        return;
    }

    const std::uint32_t verification = verification_key(key);
    auto replacement_value = [this](const PackedEntry& entry) {
        if (entry.empty()) {
            return std::numeric_limits<int>::min();
        }

        const int age = (generation_ - entry.generation()) & 0x3f;
        const int exact_bonus = (entry.generation_bound & 0x3) == static_cast<int>(Bound::Exact) ? 3 : 0;
        return entry.depth * 4 + exact_bonus - age * 8;
    };

    Cluster& cluster = cluster_for(key);
    PackedEntry* target = &cluster.entries[0];
    for (PackedEntry& entry : cluster.entries) {
        if (!entry.empty() && entry.key == verification) {
            target = &entry;
            break;
        }
        if (replacement_value(entry) < replacement_value(*target)) {
            target = &entry;
        }
    }

    const int stored_depth = std::clamp(depth + depth_offset, 1, 255);
    const bool same_position = !target->empty() && target->key == verification;
    // Keep a clearly deeper result for the same position unless the new one
    // is exact (shallow quiescence results must not erase main-search work).
    if (same_position && bound != Bound::Exact && target->depth > stored_depth + 3) {
        return;
    }

    std::uint16_t move = 0;
    if (best_move.is_valid()) {
        move = static_cast<std::uint16_t>(
            best_move.from | (best_move.to << 6) | (static_cast<int>(best_move.promotion) << 12));
    } else if (same_position) {
        move = target->move;
    }

    *target = PackedEntry{
        .key = verification,
        .move = move,
        .score = static_cast<std::int16_t>(score),
        .eval = static_cast<std::int16_t>(eval),
        .depth = static_cast<std::uint8_t>(stored_depth),
        .generation_bound = static_cast<std::uint8_t>((generation_ << 2) | static_cast<int>(bound)),
    };
}

Search::Search()
    : table_(std::make_shared<TranspositionTable>(32))
{
}

Search::Search(std::shared_ptr<TranspositionTable> table)
    : table_(std::move(table))
    , helper_(true)
{
}

Search::~Search() = default;

void Search::set_threads(int threads)
{
    const auto helper_count = static_cast<std::size_t>(std::max(1, threads) - 1);
    while (helpers_.size() > helper_count) {
        helpers_.pop_back();
    }
    while (helpers_.size() < helper_count) {
        auto helper = std::unique_ptr<Search>(new Search(table_));
        helper->shared_stop_ = &helpers_stop_;
        helper->nnue_.share_network(nnue_);
        helper->use_nnue_ = use_nnue_;
        helpers_.push_back(std::move(helper));
    }
}

int Search::threads() const
{
    return static_cast<int>(helpers_.size()) + 1;
}

std::uint64_t Search::total_nodes() const
{
    std::uint64_t total = node_count();
    for (const auto& helper : helpers_) {
        total += helper->node_count();
    }
    return total;
}

SearchResult Search::find_best_move(Board& board, int depth)
{
    SearchLimits limits;
    limits.max_depth = depth;
    return find_best_move(board, limits);
}

SearchResult Search::find_best_move(Board& board, const SearchLimits& limits)
{
    nodes_.store(0, std::memory_order_relaxed);
    tt_hits_ = 0;
    stop_ = false;
    max_nodes_ = limits.max_nodes;
    set_time_limits(limits.soft_time, limits.move_time);
    if (!helper_) {
        table_->new_search();
    }
    clear_killers();
    reset_position_history(board, limits);
    refresh_nnue(board);
    if (limits.on_search_started) {
        limits.on_search_started();
    }

    std::vector<SearchThread> helper_threads;
    helpers_stop_ = false;
    for (const auto& helper : helpers_) {
        helper_threads.emplace_back([helper = helper.get(), board, history = limits.position_history]() mutable {
            SearchLimits helper_limits;
            helper_limits.max_depth = max_search_depth;
            helper_limits.position_history = std::move(history);
            (void)helper->find_best_move(board, helper_limits);
        });
    }

    SearchResult best_completed;
    const int max_depth = std::clamp(limits.max_depth, 1, max_search_depth);
    int previous_score = 0;
    int stable_iterations = 0;

    // Depth 1 always runs, so that even a search stopped right away returns
    // a legal move.
    for (int depth = 1; depth <= max_depth; ++depth) {
        if (depth > 1 && should_stop(true)) {
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
            iteration.nodes = total_nodes();
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

        double time_scale = 1.0;
        if (iteration.best_move || (!stop_ && !best_completed.best_move)) {
            const bool same_best = best_completed.best_move && iteration.best_move
                && same_move(*best_completed.best_move, *iteration.best_move);
            stable_iterations = same_best ? stable_iterations + 1 : 0;

            // Use less time once the best move has settled, more when it just
            // changed or the score is dropping.
            if (depth > 1) {
                time_scale = stable_iterations == 0 ? 1.3 : (stable_iterations >= 4 ? 0.7 : (stable_iterations >= 2 ? 0.85 : 1.0));
                if (iteration.score < previous_score - 30) {
                    time_scale *= 1.2;
                }
            }

            previous_score = iteration.score;
            best_completed = iteration;
            if (limits.on_depth_finished) {
                limits.on_depth_finished(best_completed);
            }
        }

        if (should_stop(true) || soft_limit_reached(time_scale)) {
            break;
        }
    }

    helpers_stop_ = true;
    for (SearchThread& thread : helper_threads) {
        thread.join();
    }

    best_completed.nodes = total_nodes();
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
    soft_start_ = std::chrono::steady_clock::now().time_since_epoch().count();
    soft_deadline_ = soft_time ? deadline_after(*soft_time) : 0;
    hard_deadline_ = hard_time ? deadline_after(*hard_time) : 0;
}

void Search::set_hash_size_mb(std::size_t megabytes)
{
    table_->resize(megabytes);
}

void Search::clear_hash()
{
    table_->clear();
}

void Search::clear()
{
    table_->clear();
    clear_killers();
    for (auto& row : history_) {
        row.fill(0);
    }
    for (auto& table : *continuation_history_) {
        for (auto& row : table) {
            row.fill(0);
        }
    }
    for (auto& table : *correction_history_) {
        table.fill(0);
    }
    for (const auto& helper : helpers_) {
        helper->clear();
    }
}

int Search::hashfull() const
{
    return table_->hashfull();
}

std::size_t Search::hash_size_mb() const
{
    return table_->size_mb();
}

void Search::set_use_nnue(bool enabled)
{
    use_nnue_ = enabled;
    for (const auto& helper : helpers_) {
        helper->use_nnue_ = enabled;
    }
}

bool Search::use_nnue() const
{
    return use_nnue_;
}

bool Search::load_nnue(std::string_view path, std::string* error)
{
    const bool loaded = nnue_.load(path, error);
    for (const auto& helper : helpers_) {
        helper->nnue_.share_network(nnue_);
    }
    return loaded;
}

void Search::clear_nnue()
{
    nnue_.clear();
    for (const auto& helper : helpers_) {
        helper->nnue_.clear();
    }
}

bool Search::nnue_loaded() const
{
    return nnue_.loaded();
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
    static_evals_[0] = in_check ? no_eval : evaluate(board);
    const auto entry = table_->probe(board.hash());
    MovePicker picker = ordered_moves(board, entry ? std::optional<Move>{entry->best_move} : std::nullopt, 0);
    MoveList moves;
    while (const auto move = picker.next()) {
        moves.push_back(*move);
    }
    if (moves.empty()) {
        result.nodes = total_nodes();
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
        result.nodes = total_nodes();
        result.tt_hits = tt_hits_;
        return result;
    }

    int best_score = -infinity;
    const int original_alpha = alpha;
    const int original_beta = beta;
    bool searched_move = false;

    for (std::size_t index = 0; index < moves.size(); ++index) {
        if (searched_move && should_stop(true)) {
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
            record_cutoff(move, depth, 0, {}, {});
            break;
        }
    }

    if (!searched_move) {
        result.score = in_check ? -mate_score : 0;
        result.nodes = total_nodes();
        result.tt_hits = tt_hits_;
        return result;
    }

    result.nodes = total_nodes();
    result.tt_hits = tt_hits_;
    if (result.best_move) {
        Bound bound = Bound::Exact;
        if (best_score <= original_alpha) {
            bound = Bound::Upper;
        } else if (best_score >= original_beta) {
            bound = Bound::Lower;
        }

        table_->store(board.hash(), depth, score_to_table(result.score, 0), tt_no_eval, bound, *result.best_move);
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
        const auto entry = table_->probe(board.hash());
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

int Search::negamax(Board& board, int depth, int ply, int alpha, int beta, Move previous_move, Move excluded_move)
{
    // A singular-extension verification search of this node without one move.
    const bool singular_search = excluded_move.is_valid();
    count_node();
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
    int raw_eval = tt_no_eval;
    std::optional<TranspositionEntry> tt_entry = table_->probe(board.hash());
    const auto current_static_eval = [&]() {
        if (!static_eval) {
            if (in_check) {
                static_eval = evaluate(board);
            } else {
                raw_eval = tt_entry && tt_entry->eval != tt_no_eval ? tt_entry->eval : raw_evaluate(board);
                int eval = corrected_eval(board, raw_eval);
                // A stored search score is a better estimate when its bound
                // points the right way.
                if (tt_entry && std::abs(tt_entry->score) < mate_bound) {
                    const int tt_score = tt_entry->score;
                    if ((tt_entry->bound == Bound::Lower && tt_score > eval)
                        || (tt_entry->bound == Bound::Upper && tt_score < eval)
                        || tt_entry->bound == Bound::Exact) {
                        eval = tt_score;
                    }
                }
                static_eval = eval;
            }
        }
        return *static_eval;
    };

    std::optional<Move> tt_move;
    if (const auto& entry = tt_entry) {
        tt_move = entry->best_move;
        if (entry->depth >= depth && !singular_search) {
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

    // Static evaluation history along the current line: a position whose
    // eval improved over two plies ago is pruned less aggressively.
    const bool pv_node = original_beta - original_alpha > 1;
    static_evals_[ply] = in_check ? no_eval : current_static_eval();
    const bool improving = !in_check && ply >= 2 && static_evals_[ply - 2] != no_eval
        && static_evals_[ply] > static_evals_[ply - 2];

    if (!tt_move && depth >= 5 && ply > 0 && !in_check && beta == alpha + 1) {
        --depth;
    }

    if (!singular_search && can_try_razoring(depth, ply, alpha, beta, in_check, current_static_eval())) {
        const int score = quiescence(board, ply, alpha, beta, 0);
        if (score <= alpha) {
            return score;
        }
    }

    if (!in_check
        && !pv_node
        && !singular_search
        && can_prune_reverse_futility(depth, ply, beta, in_check, improving, current_static_eval())) {
        return current_static_eval() - reverse_futility_margin(depth, improving);
    }

    if (!singular_search && can_try_probcut(depth, ply, beta, in_check, current_static_eval())) {
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

    if (previous_move.is_valid()
        && !singular_search
        && current_static_eval() >= beta
        && can_try_null_move(board, depth, ply, alpha, beta)) {
        Board::UndoState undo;
        if (board.make_null_move(undo)) {
            push_nnue_null();
            const int reduction = 3 + depth / 3 + std::min((current_static_eval() - beta) / 200, 3);
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
    MoveList quiets_tried;
    while (const auto next_move = moves.next()) {
        const Move& move = *next_move;
        if (should_stop()) {
            break;
        }
        if (singular_search && same_move(move, excluded_move)) {
            continue;
        }

        // Prune before making the move: cheaper than making and unmaking it.
        const bool gives_check = board.gives_check(move);
        const bool capture = is_capture_like(move);
        const int see_gain = capture ? static_exchange_evaluation(board, move) : 0;
        const int move_index = searched_moves + 1;
        if (searched_move && !in_check && best > -mate_bound) {
            if (is_quiet(move)
                && can_prune_futility(move, depth, move_index, alpha, in_check, gives_check, current_static_eval())) {
                continue;
            }
            if (!pv_node && can_prune_late_move(move, depth, move_index, ply, in_check, gives_check, improving, previous_move)) {
                continue;
            }
            // SEE pruning: skip captures that lose material and quiet moves
            // that hang the moved piece, with a depth-scaled tolerance.
            if (depth <= 8 && capture && see_gain < -100 * depth) {
                continue;
            }
            if (depth <= 6 && is_quiet(move) && !gives_check && static_exchange_evaluation(board, move) < -60 * depth) {
                continue;
            }
        }

        // Singular extensions: if every alternative to a reliable TT move
        // fails well below its score, the TT move is extended. If even the
        // alternatives beat beta, several moves refute this node (multi-cut).
        int singular_extension = 0;
        if (!singular_search
            && ply > 0
            && depth >= 8
            && tt_move
            && same_move(move, *tt_move)
            && tt_entry
            && tt_entry->depth >= depth - 3
            && tt_entry->bound != Bound::Upper
            && std::abs(tt_entry->score) < mate_bound) {
            const int singular_beta = score_from_table(tt_entry->score, ply) - 2 * depth;
            const int score = negamax(board, (depth - 1) / 2, ply, singular_beta - 1, singular_beta, previous_move, move);
            if (stop_) {
                break;
            }
            if (score < singular_beta) {
                singular_extension = 1;
            } else if (singular_beta >= beta) {
                return singular_beta;
            }
        }

        Board::UndoState undo;
        if (!make_legal_move(board, move, undo)) {
            continue;
        }

        push_position(board);
        push_nnue(board, undo);

        ++searched_moves;
        const int extension = std::max(singular_extension, tactical_extension(move, depth, ply, gives_check, see_gain));
        const int full_depth = std::max(0, depth - 1 + extension);
        const int reduction = searched_move
            ? late_move_reduction(move, depth, ply, searched_moves, in_check, gives_check, pv_node, improving, previous_move)
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
            record_cutoff(move, depth, ply, previous_move, quiets_tried);
            break;
        }
        if (is_quiet(move)) {
            quiets_tried.push_back(move);
        }
    }

    if (!searched_move) {
        if (singular_search) {
            return alpha;
        }
        if (in_check) {
            return -mate_score + ply;
        }
        return 0;
    }

    if (!stop_ && !singular_search && best_move.is_valid()) {
        Bound bound = Bound::Exact;
        if (best <= original_alpha) {
            bound = Bound::Upper;
        } else if (best >= original_beta) {
            bound = Bound::Lower;
        }

        table_->store(board.hash(), depth, score_to_table(best, ply), raw_eval, bound, best_move);
        if (bound == Bound::Exact && !in_check) {
            update_correction_history(board, depth, best);
        }
    }

    return best;
}

int Search::quiescence(Board& board, int ply, int alpha, int beta, int quiet_check_depth)
{
    count_node();
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

    const auto tt_entry = table_->probe(board.hash());
    if (tt_entry) {
        const int tt_score = score_from_table(tt_entry->score, ply);
        if (tt_entry->bound == Bound::Exact
            || (tt_entry->bound == Bound::Lower && tt_score >= beta)
            || (tt_entry->bound == Bound::Upper && tt_score <= alpha)) {
            ++tt_hits_;
            return tt_score;
        }
    }

    const int original_alpha = alpha;
    const bool in_check = board.in_check(board.side_to_move());
    int best = -infinity;
    int raw_eval = tt_no_eval;
    if (!in_check) {
        raw_eval = tt_entry && tt_entry->eval != tt_no_eval ? tt_entry->eval : raw_evaluate(board);
        const int stand_pat = corrected_eval(board, raw_eval);
        if (stand_pat >= beta) {
            return stand_pat;
        }
        best = stand_pat;
        alpha = std::max(alpha, stand_pat);
    }
    Move best_move;

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

        // Quiet moves are only searched here when they give check.
        if (!in_check && !is_tactical(move) && !board.gives_check(move)) {
            continue;
        }

        Board::UndoState undo;
        if (!make_legal_move(board, move, undo)) {
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

        if (score > best) {
            best = score;
            best_move = move;
        }
        if (score >= beta) {
            break;
        }
        alpha = std::max(alpha, score);
    }

    if (in_check && !searched_move && !stop_) {
        return -mate_score + ply;
    }

    if (!stop_) {
        const Bound bound = best >= beta ? Bound::Lower : (best > original_alpha ? Bound::Exact : Bound::Upper);
        table_->store(board.hash(), 0, score_to_table(best, ply), raw_eval, bound, best_move);
    }
    return best;
}

int Search::evaluate(const Board& board) const
{
    return corrected_eval(board, raw_evaluate(board));
}

int Search::corrected_eval(const Board& board, int raw_eval) const
{
    return std::clamp(raw_eval + correction(board), -mate_score + 1000, mate_score - 1000);
}

int Search::raw_evaluate(const Board& board) const
{
    if (can_use_nnue()) {
        // Keep network scores clear of mate scores and within the int16
        // eval stored in the transposition table.
        return std::clamp(nnue_.evaluate(board), -mate_score + 1000, mate_score - 1000);
    }

    return evaluate_side_to_move(board);
}

int Search::correction(const Board& board) const
{
    return correction_entry(board);
}

std::size_t Search::correction_index(const Board& board)
{
    std::uint64_t key = board.pieces(Color::White, PieceType::Pawn) * 0x9E3779B97F4A7C15ULL;
    key ^= board.pieces(Color::Black, PieceType::Pawn) * 0xC2B2AE3D27D4EB4FULL;
    key ^= key >> 29;
    return static_cast<std::size_t>(key % correction_history_size);
}

int& Search::correction_entry(const Board& board)
{
    return (*correction_history_)[color_index(board.side_to_move())][correction_index(board)];
}

int Search::correction_entry(const Board& board) const
{
    return (*correction_history_)[color_index(board.side_to_move())][correction_index(board)];
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

    if (shared_stop_ != nullptr && shared_stop_->load(std::memory_order_relaxed)) {
        stop_ = true;
        return true;
    }

    if (max_nodes_ && node_count() >= *max_nodes_) {
        stop_ = true;
        return true;
    }

    if (!force_check && (node_count() & 2047ULL) != 0) {
        return false;
    }

    const std::int64_t deadline = hard_deadline_.load(std::memory_order_relaxed);
    if (deadline != 0 && std::chrono::steady_clock::now().time_since_epoch().count() >= deadline) {
        stop_ = true;
    }
    return stop_.load(std::memory_order_relaxed);
}

bool Search::soft_limit_reached(double scale) const
{
    const std::int64_t deadline = soft_deadline_.load(std::memory_order_relaxed);
    if (deadline == 0) {
        return false;
    }
    const std::int64_t start = soft_start_.load(std::memory_order_relaxed);
    const auto scaled = start + static_cast<std::int64_t>(static_cast<double>(deadline - start) * scale);
    return std::chrono::steady_clock::now().time_since_epoch().count() >= scaled;
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

bool Search::can_prune_reverse_futility(int depth, int ply, int beta, bool in_check, bool improving, int static_eval) const
{
    if (depth > 8 || ply <= 0 || in_check) {
        return false;
    }
    if (beta >= mate_bound || beta <= -mate_bound) {
        return false;
    }
    return static_eval - reverse_futility_margin(depth, improving) >= beta;
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
    bool improving,
    Move previous_move) const
{
    if (depth > 6 || in_check || gives_check || !is_quiet(move)) {
        return false;
    }
    if (ply >= 0 && ply < max_ply && (same_move(move, killer_moves_[ply][0]) || same_move(move, killer_moves_[ply][1]))) {
        return false;
    }
    if (quiet_history_score(move, previous_move) > 0) {
        return false;
    }

    const int threshold = (3 + depth * depth) / (improving ? 1 : 2);
    return move_index > threshold + 2;
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
    bool pv_node,
    bool improving,
    Move previous_move) const
{
    if (depth < 3 || ply <= 0 || in_check || move_index < (pv_node ? 4 : 3)) {
        return 0;
    }
    if (!is_quiet(move)) {
        return 0;
    }

    int reduction = lmr_table[std::min(depth, 63)][std::min(move_index, 63)];
    reduction += pv_node ? -1 : 0;
    reduction += improving ? 0 : 1;
    reduction -= gives_check ? 1 : 0;
    if (ply < max_ply && (same_move(move, killer_moves_[ply][0]) || same_move(move, killer_moves_[ply][1]))) {
        --reduction;
    }
    reduction -= quiet_history_score(move, previous_move) / 6000;

    return std::clamp(reduction, 0, depth - 2);
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

    return (*continuation_history_)[previous_move.to][move.from][move.to] / 2;
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
        nnue_.push(nnue::dirty_move_from(board, undo));
    }
}

void Search::push_nnue_null()
{
    if (can_use_nnue()) {
        nnue_.push(nnue::DirtyMove{});
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
    return use_nnue_ && nnue_.loaded();
}

void Search::clear_killers()
{
    for (auto& ply_killers : killer_moves_) {
        ply_killers = {};
    }
}

void Search::record_cutoff(const Move& move, int depth, int ply, Move previous_move, const MoveList& quiets_tried)
{
    if (is_tactical(move)) {
        return;
    }

    if (ply >= 0 && ply < max_ply && !same_move(move, killer_moves_[ply][0])) {
        killer_moves_[ply][1] = killer_moves_[ply][0];
        killer_moves_[ply][0] = move;
    }

    // History gravity: entries move towards the bonus and saturate smoothly
    // instead of being clamped.
    constexpr int history_limit = 16'384;
    auto update = [](int& entry, int bonus) {
        entry += bonus - entry * std::abs(bonus) / history_limit;
    };
    const int bonus = std::min(16 * depth * depth, 1200);
    const bool has_previous = previous_move.is_valid();

    update(history_[move.from][move.to], bonus);
    if (has_previous) {
        update((*continuation_history_)[previous_move.to][move.from][move.to], bonus * 2);
    }

    // Quiet moves searched before the cutoff move failed to cut off.
    for (const Move& tried : quiets_tried) {
        update(history_[tried.from][tried.to], -bonus);
        if (has_previous) {
            update((*continuation_history_)[previous_move.to][tried.from][tried.to], -bonus * 2);
        }
    }
}

void Search::update_correction_history(const Board& board, int depth, int search_score)
{
    if (depth < 2 || search_score >= mate_score - 1000 || search_score <= -mate_score + 1000) {
        return;
    }

    const int target = std::clamp(search_score - raw_evaluate(board), -256, 256);
    int& entry = correction_entry(board);
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
