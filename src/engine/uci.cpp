#include "engine/uci.hpp"

#include "engine/perft.hpp"
#include "engine/search.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <iostream>
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

int parse_depth(const std::vector<std::string>& tokens, int fallback)
{
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "depth") {
            try {
                return std::max(1, std::stoi(tokens[index + 1]));
            } catch (...) {
                return fallback;
            }
        }
    }
    return fallback;
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
            output << "uciok\n";
        } else if (command == "isready") {
            output << "readyok\n";
        } else if (command == "ucinewgame") {
            board_ = Board::startpos();
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
        } else if (command == "quit") {
            break;
        } else if (command == "stop") {
            output << "bestmove 0000\n";
        }

        output.flush();
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
    const int depth = parse_depth(tokens, 4);
    Search search;

    const auto start = std::chrono::steady_clock::now();
    SearchResult result = search.find_best_move(board_, depth);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::max<std::int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    output << "info depth " << result.depth
           << " score cp " << result.score
           << " nodes " << result.nodes
           << " time " << elapsed_ms
           << " nps " << (result.nodes * 1000 / elapsed_ms);

    if (!result.principal_variation.empty()) {
        output << " pv " << pv_to_string(result.principal_variation);
    }

    output << '\n';
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
            return board_.make_move(legal_move, undo);
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

int run_bench_command(std::ostream& output)
{
    struct BenchPosition {
        std::string_view fen;
        int depth;
    };

    const std::vector<BenchPosition> positions{
        {Board::start_fen, 4},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4},
    };

    std::uint64_t total_nodes = 0;
    const auto start = std::chrono::steady_clock::now();

    for (const auto& position : positions) {
        auto board = Board::from_fen(position.fen);
        if (!board) {
            continue;
        }
        const auto nodes = perft(*board, position.depth);
        total_nodes += nodes;
        output << "bench depth " << position.depth << " nodes " << nodes << '\n';
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ms = std::max<std::int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    output << "bench total nodes " << total_nodes
           << " time " << elapsed_ms
           << " nps " << (total_nodes * 1000 / elapsed_ms) << '\n';
    return 0;
}

} // namespace aetherz
