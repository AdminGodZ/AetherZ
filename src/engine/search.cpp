#include "engine/search.hpp"

#include "engine/evaluate.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <sstream>

namespace aetherz {
namespace {

constexpr int infinity = 32000;
constexpr int mate_score = 30000;

bool is_tactical(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::Promotion
        || move.flag == MoveFlag::PromotionCapture;
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

    if (limits.move_time) {
        deadline_ = std::chrono::steady_clock::now() + *limits.move_time;
    }

    const auto root_entry = table_.probe(board.hash());
    const auto moves = ordered_moves(
        board,
        root_entry ? std::optional<Move>{root_entry->best_move} : std::nullopt,
        0);
    if (moves.empty()) {
        SearchResult result;
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
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
        iteration.tt_hits = tt_hits_;

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

SearchResult Search::search_depth(Board& board, int depth)
{
    SearchResult result;
    result.depth = depth;

    const auto entry = table_.probe(board.hash());
    auto moves = ordered_moves(board, entry ? std::optional<Move>{entry->best_move} : std::nullopt, 0);
    if (moves.empty()) {
        result.nodes = nodes_;
        result.tt_hits = tt_hits_;
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
    result.tt_hits = tt_hits_;
    if (result.best_move) {
        result.principal_variation.push_back(*result.best_move);
        table_.store(board.hash(), depth, score_to_table(result.score, 0), Bound::Exact, *result.best_move);
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

    const int original_alpha = alpha;
    const int original_beta = beta;
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

    const auto moves = ordered_moves(board, tt_move, ply);
    if (moves.empty()) {
        if (board.in_check(board.side_to_move())) {
            return -mate_score + ply;
        }
        return 0;
    }

    int best = -infinity;
    Move best_move;
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

    if (!stop_) {
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

    const int stand_pat = evaluate(board);
    if (stand_pat >= beta) {
        return beta;
    }
    alpha = std::max(alpha, stand_pat);

    for (const Move& move : ordered_moves(board, std::nullopt, ply)) {
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
    return evaluate_side_to_move(board);
}

std::vector<Move> Search::ordered_moves(const Board& board, std::optional<Move> tt_move, int ply) const
{
    auto moves = board.generate_legal_moves();
    std::stable_sort(moves.begin(), moves.end(), [&](const Move& left, const Move& right) {
        const auto score_move = [&](const Move& move) {
            if (tt_move && same_move(move, *tt_move)) {
                return 2'000'000;
            }

            int score = 0;
            if (move.flag == MoveFlag::Promotion || move.flag == MoveFlag::PromotionCapture) {
                score += 900'000 + promotion_score(move.promotion);
            }
            if (move.flag == MoveFlag::Capture || move.flag == MoveFlag::EnPassant || move.flag == MoveFlag::PromotionCapture) {
                score += 800'000 + capture_score(board, move);
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

            return score;
        };

        return score_move(left) > score_move(right);
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
