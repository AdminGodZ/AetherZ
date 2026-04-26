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
constexpr int mate_score = 30000;
constexpr int initial_aspiration_window = 35;

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
    const std::size_t entry_count = std::max<std::size_t>(1, bytes / sizeof(TranspositionEntry));
    entries_.assign(entry_count, TranspositionEntry{});
}

void TranspositionTable::clear()
{
    std::fill(entries_.begin(), entries_.end(), TranspositionEntry{});
}

std::size_t TranspositionTable::size_mb() const
{
    return entries_.size() * sizeof(TranspositionEntry) / (1024ULL * 1024ULL);
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key) const
{
    if (entries_.empty()) {
        return std::nullopt;
    }

    const TranspositionEntry& entry = entries_[key % entries_.size()];
    if (entry.depth < 0 || entry.key != key) {
        return std::nullopt;
    }

    return entry;
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, Bound bound, Move best_move)
{
    if (entries_.empty()) {
        return;
    }

    TranspositionEntry& entry = entries_[key % entries_.size()];
    if (entry.depth >= 0 && entry.key != key && entry.depth > depth) {
        return;
    }

    entry = TranspositionEntry{
        .key = key,
        .best_move = best_move,
        .score = score,
        .depth = depth,
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
    deadline_.reset();
    clear_killers();
    reset_position_history(board, limits);

    if (limits.move_time) {
        deadline_ = std::chrono::steady_clock::now() + *limits.move_time;
    }

    SearchResult best_completed;
    const int max_depth = std::max(1, limits.max_depth);
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
        while (true) {
            iteration = search_depth(board, depth, alpha, beta);
            iteration.nodes = nodes_;
            iteration.tt_hits = tt_hits_;

            if (stop_ || depth == 1 || (iteration.score > alpha && iteration.score < beta)) {
                break;
            }

            if (iteration.score <= alpha) {
                alpha = std::max(-infinity, previous_score - window * 2);
            } else if (iteration.score >= beta) {
                beta = std::min(infinity, previous_score + window * 2);
            }

            window *= 2;
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

        if (should_stop(true)) {
            break;
        }
    }

    best_completed.nodes = nodes_;
    best_completed.tt_hits = tt_hits_;
    return best_completed;
}

void Search::set_hash_size_mb(std::size_t megabytes)
{
    table_.resize(megabytes);
}

void Search::clear_hash()
{
    table_.clear();
}

std::size_t Search::hash_size_mb() const
{
    return table_.size_mb();
}

SearchResult Search::search_depth(Board& board, int depth, int alpha, int beta)
{
    SearchResult result;
    result.depth = depth;

    const bool in_check = board.in_check(board.side_to_move());
    const auto entry = table_.probe(board.hash());
    auto moves = ordered_moves(board, entry ? std::optional<Move>{entry->best_move} : std::nullopt, 0);
    if (moves.empty()) {
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
        result.score = in_check ? -mate_score : 0;
        return result;
    }

    if (is_draw(board)) {
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
        Board::UndoState undo;
        if (!make_legal_move(board, move, undo)) {
            continue;
        }
        const bool gives_check = board.in_check(board.side_to_move());
        push_position(board);

        const int child_depth = std::max(0, depth - 1 + check_extension(move, depth, 0, gives_check));
        int score = 0;
        if (!searched_move) {
            score = -negamax(board, child_depth, 1, -beta, -alpha);
        } else {
            score = -negamax(board, child_depth, 1, -alpha - 1, -alpha);
            if (!stop_ && score > alpha && score < beta) {
                score = -negamax(board, child_depth, 1, -beta, -alpha);
            }
        }
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
            record_cutoff(move, depth, 0);
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

int Search::negamax(Board& board, int depth, int ply, int alpha, int beta)
{
    ++nodes_;
    if (should_stop()) {
        return 0;
    }
    if (is_draw(board)) {
        return 0;
    }

    if (depth <= 0) {
        return quiescence(board, ply, alpha, beta);
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

    if (!in_check
        && depth <= 3
        && beta < mate_score - 1000
        && beta > -mate_score + 1000
        && can_prune_reverse_futility(depth, ply, beta, in_check, current_static_eval())) {
        return current_static_eval() - reverse_futility_margin(depth);
    }

    if (can_try_null_move(board, depth, ply, alpha, beta)) {
        Board::UndoState undo;
        if (board.make_null_move(undo)) {
            const int reduction = 2 + depth / 4;
            const int score = -negamax(board, depth - 1 - reduction, ply + 1, -beta, -beta + 1);
            board.unmake_move(undo);

            if (score >= beta) {
                return beta;
            }
        }
    }

    const auto moves = ordered_moves(board, tt_move, ply);
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
    for (const Move& move : moves) {
        if (should_stop()) {
            break;
        }

        Board::UndoState undo;
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

        push_position(board);

        ++searched_moves;
        const int extension = check_extension(move, depth, ply, gives_check);
        const int full_depth = std::max(0, depth - 1 + extension);
        const int reduction = searched_move
            ? late_move_reduction(move, depth, ply, searched_moves, in_check, gives_check)
            : 0;
        const int child_depth = std::max(0, full_depth - reduction);

        int score = 0;
        if (!searched_move) {
            score = -negamax(board, child_depth, ply + 1, -beta, -alpha);
        } else {
            score = -negamax(board, child_depth, ply + 1, -alpha - 1, -alpha);
            if (reduction > 0 && !stop_ && score > alpha) {
                score = -negamax(board, full_depth, ply + 1, -alpha - 1, -alpha);
            }
            if (!stop_ && score > alpha && score < beta) {
                score = -negamax(board, full_depth, ply + 1, -beta, -alpha);
            }
        }
        pop_position();
        board.unmake_move(undo);
        searched_move = true;

        if (score > best) {
            best = score;
            best_move = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            record_cutoff(move, depth, ply);
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
    }

    return best;
}

int Search::quiescence(Board& board, int ply, int alpha, int beta)
{
    ++nodes_;
    if (should_stop()) {
        return 0;
    }
    if (is_draw(board)) {
        return 0;
    }

    const bool in_check = board.in_check(board.side_to_move());
    if (!in_check) {
        const int stand_pat = evaluate(board);
        if (stand_pat >= beta) {
            return beta;
        }
        alpha = std::max(alpha, stand_pat);
    }

    bool searched_move = false;
    const MoveGeneration generation = in_check ? MoveGeneration::All : MoveGeneration::Tactical;
    for (const Move& move : ordered_moves(board, std::nullopt, ply, generation)) {
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
        push_position(board);
        searched_move = true;

        const int score = -quiescence(board, ply + 1, -beta, -alpha);
        pop_position();
        board.unmake_move(undo);

        if (score >= beta) {
            return beta;
        }
        alpha = std::max(alpha, score);
    }

    if (in_check && !searched_move && !stop_) {
        return -mate_score + ply;
    }

    return alpha;
}

int Search::evaluate(const Board& board) const
{
    return evaluate_side_to_move(board);
}

std::vector<Move> Search::ordered_moves(
    const Board& board,
    std::optional<Move> tt_move,
    int ply,
    MoveGeneration generation) const
{
    auto moves = generation == MoveGeneration::Tactical
        ? board.generate_pseudo_legal_tactical_moves()
        : board.generate_pseudo_legal_moves();
    struct ScoredMove {
        Move move;
        int score = 0;
    };

    std::vector<ScoredMove> scored_moves;
    scored_moves.reserve(moves.size());

    for (const Move& move : moves) {
        int score = 0;
        if (tt_move && same_move(move, *tt_move)) {
            score = 2'000'000;
        } else {
            if (move.flag == MoveFlag::Promotion || move.flag == MoveFlag::PromotionCapture) {
                score += 900'000 + promotion_score(move.promotion);
            }
            if (is_capture_like(move)) {
                const int see = static_exchange_evaluation(board, move);
                score += (see >= 0 ? 800'000 : 100'000) + see * 8 + capture_score(board, move);
            }

            if (ply >= 0 && ply < max_ply) {
                if (same_move(move, killer_moves_[ply][0])) {
                    score += 700'000;
                } else if (same_move(move, killer_moves_[ply][1])) {
                    score += 650'000;
                }
            }

            if (move.from >= 0 && move.to >= 0) {
                score += history_[move.from][move.to];
            }
        }

        scored_moves.push_back(ScoredMove{.move = move, .score = score});
    }

    std::stable_sort(scored_moves.begin(), scored_moves.end(), [](const ScoredMove& left, const ScoredMove& right) {
        return left.score > right.score;
    });

    for (std::size_t index = 0; index < scored_moves.size(); ++index) {
        moves[index] = scored_moves[index].move;
    }

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

bool Search::has_non_pawn_material(const Board& board, Color color) const
{
    return (board.pieces(color, PieceType::Knight)
        | board.pieces(color, PieceType::Bishop)
        | board.pieces(color, PieceType::Rook)
        | board.pieces(color, PieceType::Queen))
        != 0;
}

int Search::check_extension(const Move& move, int depth, int ply, bool gives_check) const
{
    if (!gives_check || depth > 4 || ply >= max_ply - 1) {
        return 0;
    }
    if (move.flag == MoveFlag::KingCastle || move.flag == MoveFlag::QueenCastle) {
        return 0;
    }
    return 1;
}

int Search::late_move_reduction(
    const Move& move,
    int depth,
    int ply,
    int move_index,
    bool in_check,
    bool gives_check) const
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
    if (depth >= 5 && move_index >= 8) {
        ++reduction;
    }
    if (depth >= 7 && move_index >= 14) {
        ++reduction;
    }

    return std::min(reduction, depth - 1);
}

bool Search::is_draw(const Board& board) const
{
    return board.halfmove_clock() >= 100 || repetition_count(board.hash()) >= 3;
}

int Search::repetition_count(std::uint64_t hash) const
{
    return static_cast<int>(std::count(position_history_.begin(), position_history_.end(), hash));
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

void Search::clear_killers()
{
    for (auto& ply_killers : killer_moves_) {
        ply_killers = {};
    }
}

void Search::record_cutoff(const Move& move, int depth, int ply)
{
    if (is_tactical(move)) {
        return;
    }

    if (ply >= 0 && ply < max_ply && !same_move(move, killer_moves_[ply][0])) {
        killer_moves_[ply][1] = killer_moves_[ply][0];
        killer_moves_[ply][0] = move;
    }

    if (move.from >= 0 && move.to >= 0) {
        history_[move.from][move.to] += depth * depth;
    }
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
