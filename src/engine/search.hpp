#pragma once

#include "engine/board.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aetherz {

struct SearchResult {
    std::optional<Move> best_move;
    int score = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
    std::uint64_t tt_hits = 0;
    std::vector<Move> principal_variation;
};

struct SearchLimits {
    int max_depth = 4;
    std::optional<std::chrono::milliseconds> move_time;
    std::vector<std::uint64_t> position_history;
    std::function<void(const SearchResult&)> on_depth_finished;
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
    Bound bound = Bound::Exact;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 32);

    void resize(std::size_t megabytes);
    void clear();
    [[nodiscard]] std::size_t size_mb() const;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key) const;
    void store(std::uint64_t key, int depth, int score, Bound bound, Move best_move);

private:
    std::vector<TranspositionEntry> entries_;
};

class Search {
public:
    Search();

    [[nodiscard]] SearchResult find_best_move(Board& board, int depth);
    [[nodiscard]] SearchResult find_best_move(Board& board, const SearchLimits& limits);
    void set_hash_size_mb(std::size_t megabytes);
    void clear_hash();
    [[nodiscard]] std::size_t hash_size_mb() const;

private:
    static constexpr int max_ply = 128;

    TranspositionTable table_;
    std::uint64_t nodes_ = 0;
    std::uint64_t tt_hits_ = 0;
    bool stop_ = false;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::array<std::array<Move, 2>, max_ply> killer_moves_{};
    std::array<std::array<int, 64>, 64> history_{};
    std::vector<std::uint64_t> position_history_;

    [[nodiscard]] SearchResult search_depth(Board& board, int depth, int alpha, int beta);
    [[nodiscard]] std::vector<Move> principal_variation(Board board, int depth) const;
    [[nodiscard]] int negamax(Board& board, int depth, int ply, int alpha, int beta);
    [[nodiscard]] int quiescence(Board& board, int ply, int alpha, int beta);
    [[nodiscard]] int evaluate(const Board& board) const;
    [[nodiscard]] std::vector<Move> ordered_moves(const Board& board, std::optional<Move> tt_move, int ply) const;
    [[nodiscard]] bool should_stop(bool force_check = false);
    [[nodiscard]] bool can_try_null_move(const Board& board, int depth, int ply, int alpha, int beta) const;
    [[nodiscard]] bool has_non_pawn_material(const Board& board, Color color) const;
    [[nodiscard]] bool can_reduce_late_move(const Move& move, int depth, int ply, int move_index, bool in_check) const;
    [[nodiscard]] bool is_draw(const Board& board) const;
    [[nodiscard]] int repetition_count(std::uint64_t hash) const;
    void reset_position_history(const Board& board, const SearchLimits& limits);
    void push_position(const Board& board);
    void pop_position();
    void clear_killers();
    void record_cutoff(const Move& move, int depth, int ply);
};

[[nodiscard]] std::string pv_to_string(const std::vector<Move>& moves);

} // namespace aetherz
