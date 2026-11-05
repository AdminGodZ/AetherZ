#include "engine/uci.hpp"

#include "engine/benchmark.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>

namespace aetherz {
namespace {

std::vector<std::string> split_words(const std::string& line)
{
    std::istringstream stream{line};
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string join_words(const std::vector<std::string>& words, std::size_t begin, std::size_t end)
{
    std::ostringstream result;
    for (std::size_t index = begin; index < end; ++index) {
        if (index != begin) {
            result << ' ';
        }
        result << words[index];
    }
    return result.str();
}

std::optional<int> parse_int_after(const std::vector<std::string>& tokens, std::string_view name)
{
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] != name) {
            continue;
        }

        try {
            return std::stoi(tokens[index + 1]);
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::optional<std::uint64_t> parse_u64_after(const std::vector<std::string>& tokens, std::string_view name)
{
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] != name) {
            continue;
        }

        try {
            return std::stoull(tokens[index + 1]);
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

bool has_token(const std::vector<std::string>& tokens, std::string_view name)
{
    return std::ranges::find(tokens, name) != tokens.end();
}

std::optional<bool> parse_bool(std::string_view value)
{
    if (value == "true" || value == "1" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "off") {
        return false;
    }

    return std::nullopt;
}

std::int64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start)
{
    return std::max<std::int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

std::string format_score(int score)
{
    if (const int moves = mate_in_moves(score); moves != 0) {
        return "mate " + std::to_string(moves);
    }
    return "cp " + std::to_string(score);
}

std::string format_search_info(const SearchResult& result, int hashfull, std::chrono::steady_clock::time_point start)
{
    const auto elapsed_ms = elapsed_milliseconds(start);
    std::ostringstream output;
    output << "info depth " << result.depth
           << " seldepth " << std::max(result.seldepth, result.depth)
           << " score " << format_score(result.score)
           << " nodes " << result.nodes
           << " nps " << (result.nodes * 1000 / static_cast<std::uint64_t>(elapsed_ms))
           << " hashfull " << hashfull
           << " time " << elapsed_ms;

    if (!result.principal_variation.empty()) {
        output << " pv " << pv_to_string(result.principal_variation);
    }

    output << '\n';
    return output.str();
}

} // namespace

TimePlan plan_time(int remaining_ms, int increment_ms, std::optional<int> moves_to_go, int move_overhead_ms)
{
    constexpr int default_moves_to_go = 20;
    constexpr int max_moves_to_go = 50;

    const std::int64_t available = std::max(1, remaining_ms - move_overhead_ms);
    const std::int64_t moves = std::clamp(moves_to_go.value_or(default_moves_to_go), 1, max_moves_to_go);
    const std::int64_t increment = std::max(0, increment_ms);

    std::int64_t soft = available / moves + increment * 3 / 4;
    soft = std::clamp<std::int64_t>(soft, 1, available * 6 / 10);
    std::int64_t hard = std::clamp<std::int64_t>(soft * 3, soft, available * 3 / 4);
    hard = std::max<std::int64_t>(hard, 1);

    return TimePlan{
        .soft = std::chrono::milliseconds{soft},
        .hard = std::chrono::milliseconds{hard},
    };
}

UciEngine::~UciEngine()
{
    stop_search();
}

void UciEngine::write(std::string_view text)
{
    std::lock_guard lock{output_mutex_};
    if (output_ == nullptr) {
        return;
    }
    *output_ << text;
    output_->flush();
}

void UciEngine::loop(std::istream& input, std::ostream& output)
{
    output_ = &output;

    std::string line;
    while (std::getline(input, line)) {
        const auto tokens = split_words(line);
        if (tokens.empty()) {
            continue;
        }

        const std::string& command = tokens.front();
        if (command == "uci") {
            write(
                "id name AetherZ\n"
                "id author admingod\n"
                "option name Hash type spin default 32 min 1 max 65536\n"
                "option name Threads type spin default 1 min 1 max 1\n"
                "option name Move Overhead type spin default 30 min 0 max 5000\n"
                "option name Ponder type check default false\n"
                "option name Clear Hash type button\n"
                "option name UseNNUE type check default true\n"
                "option name EvalFile type string default\n"
                "uciok\n");
        } else if (command == "isready") {
            write("readyok\n");
        } else if (command == "setoption") {
            wait_for_search();
            handle_setoption(tokens);
        } else if (command == "ucinewgame") {
            wait_for_search();
            board_ = Board::startpos();
            reset_position_history();
            search_.clear();
        } else if (command == "position") {
            wait_for_search();
            handle_position(tokens);
        } else if (command == "go") {
            stop_search();
            handle_go(tokens);
        } else if (command == "stop") {
            stop_search();
        } else if (command == "ponderhit") {
            ponderhit();
        } else if (command == "quit") {
            stop_search();
            break;
        } else {
            wait_for_search();
            std::ostringstream text;
            if (command == "perft") {
                handle_perft(tokens);
            } else if (command == "divide") {
                handle_divide(tokens);
            } else if (command == "moves") {
                handle_moves();
            } else if (command == "bench") {
                run_bench_command(tokens, text, text);
            } else if (command == "perftbench") {
                run_perft_bench_command(text);
            } else if (command == "searchbench") {
                run_search_bench_command(tokens, text, text);
            } else if (command == "tactics") {
                run_tactics_command(tokens, text, text);
            } else if (command == "selfplay") {
                run_selfplay_command(tokens, text, text);
            } else if (command == "epdtest") {
                run_epd_test_command(tokens, text, text);
            } else if (command == "arasan") {
                run_arasan_command(tokens, text, text);
            }
            write(text.str());
        }
    }

    // Input closed: finish a bounded search, but do not wait forever on an
    // infinite or ponder search.
    {
        std::lock_guard lock{state_mutex_};
        if (hold_bestmove_) {
            stop_requested_ = true;
            search_.request_stop();
        }
    }
    state_changed_.notify_all();
    wait_for_search();
    output_ = nullptr;
}

void UciEngine::stop_search()
{
    {
        std::lock_guard lock{state_mutex_};
        stop_requested_ = true;
        search_.request_stop();
    }
    state_changed_.notify_all();
    wait_for_search();
}

void UciEngine::ponderhit()
{
    {
        std::lock_guard lock{state_mutex_};
        if (!pondering_) {
            return;
        }
        pondering_ = false;
        hold_bestmove_ = false;
        if (search_started_) {
            search_.set_time_limits(ponder_plan_.soft, ponder_plan_.hard);
        }
    }
    state_changed_.notify_all();
}

void UciEngine::wait_for_search()
{
    if (search_thread_.joinable()) {
        search_thread_.join();
    }
}

void UciEngine::handle_setoption(const std::vector<std::string>& tokens)
{
    std::vector<std::string> name_parts;
    std::vector<std::string> value_parts;
    std::vector<std::string>* current = nullptr;

    for (std::size_t index = 1; index < tokens.size(); ++index) {
        if (tokens[index] == "name") {
            current = &name_parts;
            continue;
        }
        if (tokens[index] == "value") {
            current = &value_parts;
            continue;
        }
        if (current != nullptr) {
            current->push_back(tokens[index]);
        }
    }

    const std::string name = join_words(name_parts, 0, name_parts.size());
    const std::string value = join_words(value_parts, 0, value_parts.size());
    if (name.empty()) {
        return;
    }

    if (name == "Hash" && !value.empty()) {
        try {
            const int megabytes = std::clamp(std::stoi(value), 1, 65536);
            search_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
        } catch (...) {
        }
    } else if (name == "Move Overhead" && !value.empty()) {
        try {
            move_overhead_ms_ = std::clamp(std::stoi(value), 0, 5000);
        } catch (...) {
        }
    } else if (name == "Clear Hash") {
        search_.clear();
    } else if (name == "UseNNUE" && !value.empty()) {
        const auto parsed = parse_bool(value);
        if (parsed) {
            search_.set_use_nnue(*parsed);
        }
    } else if (name == "EvalFile") {
        if (value.empty()) {
            search_.clear_nnue();
            return;
        }

        std::string error;
        if (search_.load_nnue(value, &error)) {
            write("info string loaded NNUE " + search_.nnue_eval_file() + "\n");
        } else {
            write("info string failed to load NNUE: " + error + "\n");
        }
    }
}

void UciEngine::handle_position(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 2) {
        return;
    }

    std::size_t move_index = tokens.size();

    if (tokens[1] == "startpos") {
        board_ = Board::startpos();
        reset_position_history();
        move_index = 2;
    } else if (tokens[1] == "fen") {
        move_index = 2;
        while (move_index < tokens.size() && tokens[move_index] != "moves") {
            ++move_index;
        }

        const auto parsed = Board::from_fen(join_words(tokens, 2, move_index));
        if (!parsed) {
            return;
        }
        board_ = *parsed;
        reset_position_history();
    }

    if (move_index < tokens.size() && tokens[move_index] == "moves") {
        ++move_index;
    }

    for (; move_index < tokens.size(); ++move_index) {
        if (!apply_uci_move(tokens[move_index])) {
            break;
        }
    }
}

void UciEngine::handle_go(const std::vector<std::string>& tokens)
{
    const auto depth = parse_int_after(tokens, "depth");
    const auto move_time = parse_int_after(tokens, "movetime");
    const auto nodes = parse_u64_after(tokens, "nodes");
    const bool white = board_.side_to_move() == Color::White;
    const auto remaining = parse_int_after(tokens, white ? "wtime" : "btime");
    const int increment = parse_int_after(tokens, white ? "winc" : "binc").value_or(0);
    const auto moves_to_go = parse_int_after(tokens, "movestogo");
    const bool ponder = has_token(tokens, "ponder");
    const bool infinite = has_token(tokens, "infinite") || (!depth && !move_time && !nodes && !remaining);

    SearchLimits limits;
    limits.max_depth = depth ? std::max(1, *depth) : max_search_depth;
    limits.max_nodes = nodes;
    limits.position_history = position_history_;

    TimePlan plan;
    if (move_time) {
        const auto budget = std::chrono::milliseconds{std::max(1, *move_time - move_overhead_ms_)};
        plan = TimePlan{.soft = budget, .hard = budget};
    } else if (remaining && *remaining > 0) {
        plan = plan_time(*remaining, increment, moves_to_go, move_overhead_ms_);
    }

    {
        std::lock_guard lock{state_mutex_};
        hold_bestmove_ = infinite || ponder;
        pondering_ = ponder;
        ponder_search_ = ponder;
        search_started_ = false;
        stop_requested_ = false;
        ponder_plan_ = plan;
    }

    if (!ponder) {
        limits.soft_time = plan.soft;
        limits.move_time = plan.hard;
    }

    const auto start = std::chrono::steady_clock::now();
    // The search resets its stop flag and deadlines when it starts, so apply
    // a stop or ponderhit that arrived before that point now.
    limits.on_search_started = [this] {
        std::lock_guard lock{state_mutex_};
        search_started_ = true;
        if (stop_requested_) {
            search_.request_stop();
        } else if (ponder_search_ && !pondering_) {
            search_.set_time_limits(ponder_plan_.soft, ponder_plan_.hard);
        }
    };
    limits.on_depth_finished = [this, start](const SearchResult& info) {
        write(format_search_info(info, search_.hashfull(), start));
    };

    search_thread_ = std::thread{[this, limits = std::move(limits), board = board_, start]() mutable {
        SearchResult result = search_.find_best_move(board, limits);
        if (result.depth == 0) {
            write(format_search_info(result, search_.hashfull(), start));
        }

        {
            std::unique_lock lock{state_mutex_};
            state_changed_.wait(lock, [this] {
                return !hold_bestmove_ || stop_requested_;
            });
        }

        std::string text = "bestmove ";
        text += result.best_move ? move_to_uci(*result.best_move) : "0000";
        if (result.best_move && result.principal_variation.size() >= 2) {
            text += " ponder " + move_to_uci(result.principal_variation[1]);
        }
        text += '\n';
        write(text);
    }};
}

void UciEngine::handle_perft(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 2) {
        write("info string usage: perft <depth>\n");
        return;
    }

    int depth = 0;
    try {
        depth = std::stoi(tokens[1]);
    } catch (...) {
        write("info string invalid depth\n");
        return;
    }

    Board board = board_;
    write("nodes " + std::to_string(perft(board, depth)) + "\n");
}

void UciEngine::handle_divide(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 2) {
        write("info string usage: divide <depth>\n");
        return;
    }

    int depth = 0;
    try {
        depth = std::stoi(tokens[1]);
    } catch (...) {
        write("info string invalid depth\n");
        return;
    }

    Board board = board_;
    std::uint64_t total = 0;
    std::ostringstream text;
    for (const auto& line : perft_divide(board, depth)) {
        total += line.nodes;
        text << move_to_uci(line.move) << ": " << line.nodes << '\n';
    }
    text << "nodes " << total << '\n';
    write(text.str());
}

void UciEngine::handle_moves()
{
    std::ostringstream text;
    for (const Move& move : board_.generate_legal_moves()) {
        text << move_to_uci(move) << '\n';
    }
    write(text.str());
}

void UciEngine::reset_position_history()
{
    position_history_.clear();
    record_current_position();
}

void UciEngine::record_current_position()
{
    position_history_.push_back(board_.hash());
}

bool UciEngine::apply_uci_move(std::string_view move_text)
{
    const auto requested = move_from_uci(move_text);
    if (!requested) {
        return false;
    }

    for (const Move& legal_move : board_.generate_legal_moves()) {
        if (legal_move.from == requested->from
            && legal_move.to == requested->to
            && legal_move.promotion == requested->promotion) {
            Board::UndoState undo;
            if (!board_.make_move(legal_move, undo)) {
                return false;
            }
            record_current_position();
            return true;
        }
    }

    return false;
}

int run_perft_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    if (args.size() < 2) {
        error << "usage: aetherz perft <depth> [fen]\n";
        return 1;
    }

    int depth = 0;
    try {
        depth = std::stoi(args[1]);
    } catch (...) {
        error << "invalid depth\n";
        return 1;
    }

    Board board = Board::startpos();
    if (args.size() > 2) {
        const auto parsed = Board::from_fen(join_words(args, 2, args.size()));
        if (!parsed) {
            error << "invalid fen\n";
            return 1;
        }
        board = *parsed;
    }

    const auto start = std::chrono::steady_clock::now();
    const auto nodes = perft(board, depth);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    output << nodes << " nodes";
    if (elapsed_ms > 0) {
        output << " in " << elapsed_ms << " ms";
    }
    output << '\n';
    return 0;
}

int run_divide_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    if (args.size() < 2) {
        error << "usage: aetherz divide <depth> [fen]\n";
        return 1;
    }

    int depth = 0;
    try {
        depth = std::stoi(args[1]);
    } catch (...) {
        error << "invalid depth\n";
        return 1;
    }

    Board board = Board::startpos();
    if (args.size() > 2) {
        const auto parsed = Board::from_fen(join_words(args, 2, args.size()));
        if (!parsed) {
            error << "invalid fen\n";
            return 1;
        }
        board = *parsed;
    }

    std::uint64_t total = 0;
    for (const auto& line : perft_divide(board, depth)) {
        total += line.nodes;
        output << move_to_uci(line.move) << ": " << line.nodes << '\n';
    }
    output << "nodes " << total << '\n';
    return 0;
}

} // namespace aetherz
