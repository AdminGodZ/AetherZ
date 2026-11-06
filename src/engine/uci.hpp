#pragma once

#include "engine/board.hpp"
#include "engine/search.hpp"
#include "engine/thread.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

struct TimePlan {
    std::optional<std::chrono::milliseconds> soft;
    std::optional<std::chrono::milliseconds> hard;
};

// Splits the remaining clock time into a soft limit (do not start another
// iteration) and a hard limit (abort the search).
[[nodiscard]] TimePlan plan_time(
    int remaining_ms,
    int increment_ms,
    std::optional<int> moves_to_go,
    int move_overhead_ms);

class UciEngine {
public:
    // Loads the built-in network, if the engine has one.
    UciEngine();
    UciEngine(const UciEngine&) = delete;
    UciEngine& operator=(const UciEngine&) = delete;
    ~UciEngine();

    void loop(std::istream& input, std::ostream& output);

private:
    Board board_ = Board::startpos();
    Search search_;
    std::vector<std::uint64_t> position_history_{Board::startpos().hash()};
    int move_overhead_ms_ = 30;
    Search::SyzygyOptions syzygy_options_;

    std::ostream* output_ = nullptr;
    std::mutex output_mutex_;

    SearchThread search_thread_;
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    // Infinite and ponder searches must not report bestmove before the GUI
    // sends stop (or ponderhit, for ponder searches).
    bool hold_bestmove_ = false;
    bool pondering_ = false;
    bool ponder_search_ = false;
    bool search_started_ = false;
    bool stop_requested_ = false;
    TimePlan ponder_plan_;

    void write(std::string_view text);
    void handle_setoption(const std::vector<std::string>& tokens);
    void handle_position(const std::vector<std::string>& tokens);
    void handle_go(const std::vector<std::string>& tokens);
    void handle_perft(const std::vector<std::string>& tokens);
    void handle_divide(const std::vector<std::string>& tokens);
    void handle_moves();
    void stop_search();
    void ponderhit();
    void wait_for_search();
    void reset_position_history();
    void record_current_position();
    [[nodiscard]] bool apply_uci_move(std::string_view move_text);
};

int run_perft_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_divide_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
} // namespace aetherz
