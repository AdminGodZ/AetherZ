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

std::chrono::milliseconds safe_move_time(int milliseconds)
{
    constexpr int move_overhead_ms = 10;
    return std::chrono::milliseconds{std::max(1, milliseconds - move_overhead_ms)};
}

SearchLimits parse_search_limits(const std::vector<std::string>& tokens, Color side_to_move)
{
    constexpr int default_depth = 4;
    constexpr int timed_max_depth = 64;
    constexpr int default_moves_to_go = 30;

    SearchLimits limits;

    const auto depth = parse_int_after(tokens, "depth");
    const auto move_time = parse_int_after(tokens, "movetime");
    const auto white_time = parse_int_after(tokens, "wtime");
    const auto black_time = parse_int_after(tokens, "btime");
    const auto white_increment = parse_int_after(tokens, "winc").value_or(0);
    const auto black_increment = parse_int_after(tokens, "binc").value_or(0);
    const auto moves_to_go = std::max(1, parse_int_after(tokens, "movestogo").value_or(default_moves_to_go));

    if (depth) {
        limits.max_depth = std::max(1, *depth);
    } else if (move_time || white_time || black_time) {
        limits.max_depth = timed_max_depth;
    } else {
        limits.max_depth = default_depth;
    }

    if (move_time) {
        limits.move_time = safe_move_time(*move_time);
        return limits;
    }

    const auto remaining_time = side_to_move == Color::White ? white_time : black_time;
    const int increment = side_to_move == Color::White ? white_increment : black_increment;
    if (!remaining_time || *remaining_time <= 0) {
        return limits;
    }

    const int base = *remaining_time / moves_to_go;
    const int increment_bonus = increment * 3 / 4;
    const int cap = std::max(1, *remaining_time / 4);
    limits.move_time = safe_move_time(std::min(base + increment_bonus, cap));
    return limits;
}

std::int64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start)
{
    return std::max<std::int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

void write_search_info(
    std::ostream& output,
    const SearchResult& result,
    std::chrono::steady_clock::time_point start)
{
    const auto elapsed_ms = elapsed_milliseconds(start);
    output << "info depth " << result.depth
           << " score cp " << result.score
           << " nodes " << result.nodes
           << " time " << elapsed_ms
           << " nps " << (result.nodes * 1000 / elapsed_ms);

    if (!result.principal_variation.empty()) {
        output << " pv " << pv_to_string(result.principal_variation);
    }

    output << '\n';
}

} // namespace

void UciEngine::loop(std::istream& input, std::ostream& output)
{
    std::string line;
    while (std::getline(input, line)) {
        const auto tokens = split_words(line);
        if (tokens.empty()) {
            continue;
        }

        const std::string& command = tokens.front();
        if (command == "uci") {
            output << "id name AetherZ\n";
            output << "id author admingod\n";
            output << "option name Hash type spin default 32 min 1 max 1024\n";
            output << "uciok\n";
        } else if (command == "isready") {
            output << "readyok\n";
        } else if (command == "setoption") {
            handle_setoption(tokens);
        } else if (command == "ucinewgame") {
            board_ = Board::startpos();
            reset_position_history();
            search_.clear_hash();
        } else if (command == "position") {
            handle_position(tokens);
        } else if (command == "go") {
            handle_go(tokens, output);
        } else if (command == "perft") {
            handle_perft(tokens, output);
        } else if (command == "divide") {
            handle_divide(tokens, output);
        } else if (command == "moves") {
            handle_moves(output);
        } else if (command == "bench") {
            run_bench_command(output);
        } else if (command == "searchbench") {
            run_search_bench_command(tokens, output, output);
        } else if (command == "tactics") {
            run_tactics_command(tokens, output, output);
        } else if (command == "selfplay") {
            run_selfplay_command(tokens, output, output);
        } else if (command == "quit") {
            break;
        } else if (command == "stop") {
            output << "bestmove 0000\n";
        }

        output.flush();
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
    if (name != "Hash" || value.empty()) {
        return;
    }

    try {
        const int megabytes = std::clamp(std::stoi(value), 1, 1024);
        search_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
    } catch (...) {
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

void UciEngine::handle_go(const std::vector<std::string>& tokens, std::ostream& output)
{
    SearchLimits limits = parse_search_limits(tokens, board_.side_to_move());
    limits.position_history = position_history_;

    const auto start = std::chrono::steady_clock::now();
    limits.on_depth_finished = [&](const SearchResult& info) {
        write_search_info(output, info, start);
        output.flush();
    };

    SearchResult result = search_.find_best_move(board_, limits);
    if (result.depth == 0) {
        write_search_info(output, result, start);
    }

    output << "bestmove " << (result.best_move ? move_to_uci(*result.best_move) : "0000") << '\n';
}

void UciEngine::handle_perft(const std::vector<std::string>& tokens, std::ostream& output)
{
    if (tokens.size() < 2) {
        output << "info string usage: perft <depth>\n";
        return;
    }

    int depth = 0;
    try {
        depth = std::stoi(tokens[1]);
    } catch (...) {
        output << "info string invalid depth\n";
        return;
    }

    Board board = board_;
    output << "nodes " << perft(board, depth) << '\n';
}

void UciEngine::handle_divide(const std::vector<std::string>& tokens, std::ostream& output)
{
    if (tokens.size() < 2) {
        output << "info string usage: divide <depth>\n";
        return;
    }

    int depth = 0;
    try {
        depth = std::stoi(tokens[1]);
    } catch (...) {
        output << "info string invalid depth\n";
        return;
    }

    Board board = board_;
    std::uint64_t total = 0;
    for (const auto& line : perft_divide(board, depth)) {
        total += line.nodes;
        output << move_to_uci(line.move) << ": " << line.nodes << '\n';
    }
    output << "nodes " << total << '\n';
}

void UciEngine::handle_moves(std::ostream& output)
{
    for (const Move& move : board_.generate_legal_moves()) {
        output << move_to_uci(move) << '\n';
    }
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
