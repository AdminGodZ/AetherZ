#pragma once

#include "engine/board.hpp"
#include "engine/nnue/nnue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

inline constexpr int max_search_ply = 128;
inline constexpr int max_search_depth = max_search_ply - 8;
inline constexpr int mate_score = 30000;
inline constexpr int mate_bound = mate_score - max_search_ply;
static_assert(nnue::accumulator_stack_size >= max_search_ply + 8, "every search ply has an NNUE accumulator");

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
    // This thread's nodes for the iteration's last root search, and the share
    // of them spent below the best move; used for time management.
    std::uint64_t root_nodes = 0;
    std::uint64_t best_move_nodes = 0;
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

inline constexpr int tt_no_eval = -32'768;

// Decoded view of a transposition table entry.
struct TranspositionEntry {
    Move best_move;
    int score = 0;
    // Raw static evaluation, or tt_no_eval when it was not computed.
    int eval = tt_no_eval;
    int depth = 0;
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
    void store(std::uint64_t key, int depth, int score, int eval, Bound bound, Move best_move);

private:
    // 12-byte entries, five to a cache line. Only the upper 32 key bits are
    // stored; the cluster index comes from the full key.
    struct PackedEntry {
        std::uint32_t key = 0;
        std::uint16_t move = 0;
        std::int16_t score = 0;
        std::int16_t eval = 0;
        // Depth offset so that zero marks an empty entry.
        std::uint8_t depth = 0;
        // Generation in the upper six bits, bound in the lower two.
        std::uint8_t generation_bound = 0;

        [[nodiscard]] bool empty() const { return depth == 0; }
        [[nodiscard]] std::uint8_t generation() const { return generation_bound >> 2; }
    };

    static constexpr std::size_t cluster_size = 5;
    static constexpr int depth_offset = 2;

    struct alignas(64) Cluster {
        std::array<PackedEntry, cluster_size> entries{};
    };

    [[nodiscard]] static std::uint32_t verification_key(std::uint64_t key) { return static_cast<std::uint32_t>(key >> 32); }
    [[nodiscard]] Cluster& cluster_for(std::uint64_t key);
    [[nodiscard]] const Cluster& cluster_for(std::uint64_t key) const;

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
        int see = 0;
    };

    // Moves come out by stage, then by score (highest first), then by key
    // (lowest first) so the order does not depend on the generation order.
    void add(const ScoredMove& move)
    {
        moves_.values[count_++] = {.order = sort_order(move), .move = move.move, .see = move.see};
    }
    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] std::optional<Move> next();
    // Static exchange evaluation of the move next() returned last (0 unless
    // it captures).
    [[nodiscard]] int see() const { return moves_.values[index_ - 1].see; }

private:
    struct OrderedMove {
        std::uint64_t order;
        Move move;
        int see;
    };

    // Stage, score and key packed into one number that sorts the same way:
    // a stage below 2^16, a score that fits in 32 bits, a key below 2^15.
    [[nodiscard]] static std::uint64_t sort_order(const ScoredMove& move)
    {
        const auto inverted_score = static_cast<std::uint64_t>(std::int64_t{INT32_MAX} - move.score);
        return (static_cast<std::uint64_t>(move.stage) << 48) | (inverted_score << 15)
            | static_cast<std::uint64_t>(move.key);
    }

    UninitializedArray<OrderedMove, MoveList::capacity> moves_;
    std::size_t count_ = 0;
    std::size_t index_ = 0;
};

class Search {
public:
    Search();
    ~Search();
    Search(const Search&) = delete;
    Search& operator=(const Search&) = delete;

    [[nodiscard]] SearchResult find_best_move(Board& board, int depth);
    [[nodiscard]] SearchResult find_best_move(Board& board, const SearchLimits& limits);
    // Thread-safe: may be called while another thread is searching.
    void request_stop();
    void set_time_limits(
        std::optional<std::chrono::milliseconds> soft_time,
        std::optional<std::chrono::milliseconds> hard_time);

    // Lazy SMP: helper threads search the same position and share the hash
    // table; only the main thread reports results.
    void set_threads(int threads);
    [[nodiscard]] int threads() const;

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
    [[nodiscard]] std::string nnue_eval_file() const;

private:
    enum class MoveGeneration {
        All,
        Tactical,
    };

    static constexpr int max_ply = max_search_ply;
    static constexpr std::size_t correction_history_size = 16'384;

    explicit Search(std::shared_ptr<TranspositionTable> table);

    std::shared_ptr<TranspositionTable> table_;
    std::vector<std::unique_ptr<Search>> helpers_;
    // Set for helper searches: they share the main search's table and stop
    // when the main search tells them to.
    bool helper_ = false;
    std::atomic<bool> helpers_stop_ = false;
    const std::atomic<bool>* shared_stop_ = nullptr;
    // Written only by the owning thread; atomic so other threads can read it.
    std::atomic<std::uint64_t> nodes_ = 0;
    std::uint64_t tt_hits_ = 0;
    int seldepth_ = 0;
    std::atomic<bool> stop_ = false;
    // Deadlines as steady_clock ticks; zero means no deadline.
    std::atomic<std::int64_t> hard_deadline_ = 0;
    std::atomic<std::int64_t> soft_deadline_ = 0;
    std::atomic<std::int64_t> soft_start_ = 0;
    std::optional<std::uint64_t> max_nodes_;
    std::array<std::array<Move, 2>, max_ply> killer_moves_{};
    std::array<std::array<int, 64>, 64> history_{};
    // The two large tables live on the heap so that a Search object is small
    // enough for any thread's stack (the main thread gets 1-2 MB on Windows).
    using ContinuationHistory = std::array<std::array<std::array<int, 64>, 64>, 64>;
    std::unique_ptr<ContinuationHistory> continuation_history_ = std::make_unique<ContinuationHistory>();
    // Static evaluation corrections keyed by pawn structure and side to move.
    using CorrectionHistory = std::array<std::array<int, correction_history_size>, 2>;
    std::unique_ptr<CorrectionHistory> correction_history_ = std::make_unique<CorrectionHistory>();
    std::vector<std::uint64_t> position_history_;
    static constexpr int no_eval = -1'000'000;
    std::array<int, max_ply + 1> static_evals_{};
    nnue::NnueEvaluator nnue_;
    bool use_nnue_ = true;

    void count_node() { nodes_.store(nodes_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t node_count() const { return nodes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t total_nodes() const;
    [[nodiscard]] SearchResult search_depth(Board& board, int depth, int alpha, int beta);
    [[nodiscard]] std::vector<Move> principal_variation(Board board, int depth) const;
    [[nodiscard]] int negamax(
        Board& board,
        int depth,
        int ply,
        int alpha,
        int beta,
        Move previous_move,
        Move excluded_move = {});
    [[nodiscard]] int quiescence(Board& board, int ply, int alpha, int beta, int quiet_check_depth);
    [[nodiscard]] int evaluate(const Board& board) const;
    [[nodiscard]] int raw_evaluate(const Board& board) const;
    [[nodiscard]] int corrected_eval(const Board& board, int raw_eval) const;
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
    [[nodiscard]] bool soft_limit_reached(double scale = 1.0) const;
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
    void push_nnue_null();
    void pop_nnue();
    [[nodiscard]] bool can_use_nnue() const;
    void clear_killers();
    void record_cutoff(const Move& move, int depth, int ply, Move previous_move, const MoveList& quiets_tried);
    void update_correction_history(const Board& board, int depth, int search_score);
};

[[nodiscard]] std::string pv_to_string(const std::vector<Move>& moves);

} // namespace aetherz
