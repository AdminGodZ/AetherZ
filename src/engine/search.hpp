#pragma once

#include "engine/board.hpp"
#include "engine/nnue/nnue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

inline constexpr int max_search_ply = 128;
inline constexpr int max_search_depth = max_search_ply - 8;
inline constexpr int mate_score = 30000;
inline constexpr int mate_bound = mate_score - max_search_ply;

// Moves until mate for a mate score (negative when getting mated), or 0 when
// the score is not a mate score.
[[nodiscard]] constexpr int mate_in_moves(int score)
{
    if (score >= mate_bound) {
        return (mate_score - score + 1) / 2;
    }
    if (score <= -mate_bound) {
        return -(mate_score + score) / 2;
    }
    return 0;
}

struct SearchResult {
    std::optional<Move> best_move;
    int score = 0;
    int depth = 0;
    int seldepth = 0;
    std::uint64_t nodes = 0;
    std::uint64_t tt_hits = 0;
    std::vector<Move> principal_variation;
};

struct SearchLimits {
    int max_depth = 4;
    // Hard limit: the search is aborted once it is reached.
    std::optional<std::chrono::milliseconds> move_time;
    // Soft limit: no new iteration is started once it is reached.
    std::optional<std::chrono::milliseconds> soft_time;
    std::optional<std::uint64_t> max_nodes;
    std::vector<std::uint64_t> position_history;
    std::function<void(const SearchResult&)> on_depth_finished;
    // Called once the limits above are in effect, before the first iteration.
    std::function<void()> on_search_started;
};

enum class Bound : std::uint8_t {
    Exact,
    Lower,
    Upper,
};

struct TranspositionEntry {
    std::uint64_t key = 0;
    Move best_move;
    int score = 0;
    int depth = -1;
    std::uint8_t generation = 0;
    Bound bound = Bound::Exact;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 32);

    void resize(std::size_t megabytes);
    void clear();
    void new_search();
    [[nodiscard]] std::size_t size_mb() const;
    [[nodiscard]] int hashfull() const;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key) const;
    void store(std::uint64_t key, int depth, int score, Bound bound, Move best_move);

private:
    static constexpr std::size_t cluster_size = 2;

    struct alignas(64) Cluster {
        std::array<TranspositionEntry, cluster_size> entries{};
    };

    std::vector<Cluster> clusters_;
    std::uint8_t generation_ = 1;
};

// Hands out scored moves best-first. The best move is selected on its own and
// the rest are only sorted when the first move does not cause a cutoff.
class MovePicker {
public:
    struct ScoredMove {
        Move move;
        int stage = 0;
        int score = 0;
        int key = 0;
    };

    void add(const ScoredMove& move) { moves_[count_++] = move; }
    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] std::optional<Move> next();

private:
    std::array<ScoredMove, MoveList::capacity> moves_;
    std::size_t count_ = 0;
    std::size_t index_ = 0;
};

class Search {
public:
    Search();

    [[nodiscard]] SearchResult find_best_move(Board& board, int depth);
    [[nodiscard]] SearchResult find_best_move(Board& board, const SearchLimits& limits);
    // Thread-safe: may be called while another thread is searching.
    void request_stop();
    void set_time_limits(
        std::optional<std::chrono::milliseconds> soft_time,
        std::optional<std::chrono::milliseconds> hard_time);

    void set_hash_size_mb(std::size_t megabytes);
    void clear_hash();
    // Clears the hash table and all move-ordering and correction histories.
    void clear();
    [[nodiscard]] int hashfull() const;
    [[nodiscard]] std::size_t hash_size_mb() const;
    void set_use_nnue(bool enabled);
    [[nodiscard]] bool use_nnue() const;
    [[nodiscard]] bool load_nnue(std::string_view path, std::string* error = nullptr);
    void clear_nnue();
    [[nodiscard]] bool nnue_loaded() const;
    [[nodiscard]] bool nnue_ready() const;
    [[nodiscard]] std::string nnue_eval_file() const;

private:
    enum class MoveGeneration {
        All,
        Tactical,
    };

    static constexpr int max_ply = max_search_ply;
    static constexpr std::size_t correction_history_size = 16'384;

    TranspositionTable table_;
    std::uint64_t nodes_ = 0;
    std::uint64_t tt_hits_ = 0;
    int seldepth_ = 0;
    std::atomic<bool> stop_ = false;
    // Deadlines as steady_clock ticks; zero means no deadline.
    std::atomic<std::int64_t> hard_deadline_ = 0;
    std::atomic<std::int64_t> soft_deadline_ = 0;
    std::optional<std::uint64_t> max_nodes_;
    std::array<std::array<Move, 2>, max_ply> killer_moves_{};
    std::array<std::array<int, 64>, 64> history_{};
    std::array<std::array<std::array<int, 64>, 64>, 64> continuation_history_{};
    // Static evaluation corrections keyed by pawn structure and side to move.
    std::array<std::array<int, correction_history_size>, 2> correction_history_{};
    std::vector<std::uint64_t> position_history_;
    static constexpr int no_eval = -1'000'000;
    std::array<int, max_ply + 1> static_evals_{};
    nnue::NnueEvaluator nnue_;
    bool use_nnue_ = true;

    [[nodiscard]] SearchResult search_depth(Board& board, int depth, int alpha, int beta);
    [[nodiscard]] std::vector<Move> principal_variation(Board board, int depth) const;
    [[nodiscard]] int negamax(Board& board, int depth, int ply, int alpha, int beta, Move previous_move);
    [[nodiscard]] int quiescence(Board& board, int ply, int alpha, int beta, int quiet_check_depth);
    [[nodiscard]] int evaluate(const Board& board) const;
    [[nodiscard]] int raw_evaluate(const Board& board) const;
    [[nodiscard]] int correction(const Board& board) const;
    [[nodiscard]] static std::size_t correction_index(const Board& board);
    [[nodiscard]] int& correction_entry(const Board& board);
    [[nodiscard]] int correction_entry(const Board& board) const;
    [[nodiscard]] MovePicker ordered_moves(
        const Board& board,
        std::optional<Move> tt_move,
        int ply,
        Move previous_move = {},
        MoveGeneration generation = MoveGeneration::All) const;
    [[nodiscard]] bool should_stop(bool force_check = false);
    [[nodiscard]] bool soft_limit_reached() const;
    [[nodiscard]] bool can_try_razoring(int depth, int ply, int alpha, int beta, bool in_check, int static_eval) const;
    [[nodiscard]] bool can_try_probcut(int depth, int ply, int beta, bool in_check, int static_eval) const;
    [[nodiscard]] bool can_try_null_move(const Board& board, int depth, int ply, int alpha, int beta) const;
    [[nodiscard]] bool can_prune_reverse_futility(
        int depth,
        int ply,
        int beta,
        bool in_check,
        bool improving,
        int static_eval) const;
    [[nodiscard]] bool can_prune_futility(
        const Move& move,
        int depth,
        int move_index,
        int alpha,
        bool in_check,
        bool gives_check,
        int static_eval) const;
    [[nodiscard]] bool can_prune_late_move(
        const Move& move,
        int depth,
        int move_index,
        int ply,
        bool in_check,
        bool gives_check,
        bool improving,
        Move previous_move) const;
    [[nodiscard]] bool has_non_pawn_material(const Board& board, Color color) const;
    [[nodiscard]] int tactical_extension(const Move& move, int depth, int ply, bool gives_check, int see_gain) const;
    [[nodiscard]] int late_move_reduction(
        const Move& move,
        int depth,
        int ply,
        int move_index,
        bool in_check,
        bool gives_check,
        bool pv_node,
        bool improving,
        Move previous_move) const;
    [[nodiscard]] bool is_draw(const Board& board, int ply) const;
    [[nodiscard]] int repetition_count(const Board& board) const;
    [[nodiscard]] int continuation_score(const Move& move, Move previous_move) const;
    [[nodiscard]] int quiet_history_score(const Move& move, Move previous_move) const;
    void reset_position_history(const Board& board, const SearchLimits& limits);
    void push_position(const Board& board);
    void pop_position();
    void refresh_nnue(const Board& board);
    void push_nnue(const Board& board, const Board::UndoState& undo);
    void push_nnue_null(const Board& board);
    void pop_nnue();
    [[nodiscard]] bool can_use_nnue() const;
    void clear_killers();
    void record_cutoff(const Move& move, int depth, int ply, Move previous_move, const MoveList& quiets_tried);
    void update_correction_history(const Board& board, int depth, int search_score);
};

[[nodiscard]] std::string pv_to_string(const std::vector<Move>& moves);

} // namespace aetherz
