#include "engine/benchmark.hpp"

#include "engine/board.hpp"
#include "engine/epd.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"
#include "engine/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {
namespace {

struct SearchBenchPosition {
    std::string_view name;
    std::string_view fen;
};

struct TacticPosition {
    std::string_view name;
    std::string_view fen;
    std::vector<std::string_view> best_moves;
};

std::optional<int> parse_positive_int(const std::vector<std::string>& args, std::size_t index)
{
    if (index >= args.size()) {
        return std::nullopt;
    }

    try {
        const int value = std::stoi(args[index]);
        if (value > 0) {
            return value;
        }
    } catch (...) {
    }

    return std::nullopt;
}

std::string join_words(const std::vector<std::string>& words)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (index != 0) {
            output << ' ';
        }
        output << words[index];
    }
    return output.str();
}

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::max<std::int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

bool same_uci_move(const Move& move, std::string_view expected)
{
    return move_to_uci(move) == expected;
}

bool contains_move(const std::vector<std::string_view>& moves, const Move& move)
{
    return std::ranges::any_of(moves, [&](std::string_view expected) {
        return same_uci_move(move, expected);
    });
}

bool contains_epd_move(const Board& board, const std::vector<std::string>& moves, const Move& move)
{
    return std::ranges::any_of(moves, [&](const std::string& expected) {
        return epd_move_matches(board, expected, move);
    });
}

std::string result_for_terminal_board(const Board& board)
{
    if (!board.generate_legal_moves().empty()) {
        return "*";
    }

    if (!board.in_check(board.side_to_move())) {
        return "1/2-1/2";
    }

    return board.side_to_move() == Color::White ? "0-1" : "1-0";
}

int repetition_count(const std::vector<std::uint64_t>& history, std::uint64_t hash)
{
    return static_cast<int>(std::count(history.begin(), history.end(), hash));
}

bool play_best_move(Board& board, Search& search, int depth, const std::vector<std::uint64_t>& history, Move& played)
{
    SearchLimits limits;
    limits.max_depth = depth;
    limits.position_history = history;

    Board search_board = board;
    const SearchResult result = search.find_best_move(search_board, limits);
    if (!result.best_move) {
        return false;
    }

    for (const Move& legal_move : board.generate_legal_moves()) {
        if (legal_move.from == result.best_move->from
            && legal_move.to == result.best_move->to
            && legal_move.promotion == result.best_move->promotion) {
            Board::UndoState undo;
            if (!board.make_move(legal_move, undo)) {
                return false;
            }
            played = legal_move;
            return true;
        }
    }

    return false;
}

const std::vector<SearchBenchPosition>& search_positions()
{
    static const std::vector<SearchBenchPosition> positions{
        {"startpos", Board::start_fen},
        {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"},
        {"middlegame", "r1bq1rk1/ppp2ppp/2nbpn2/3p4/3P4/2PBPN2/PP3PPP/RNBQ1RK1 w - - 0 8"},
        {"rook-endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"},
        {"mate-in-one", "7k/8/5KQ1/8/8/8/8/8 w - - 0 1"},
    };
    return positions;
}

// Fixed position set for the deterministic search benchmark. Its total node
// count acts as a search signature: refactors must keep it unchanged.
const std::vector<std::string_view>& bench_fens()
{
    static const std::vector<std::string_view> fens{
        Board::start_fen,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "r1bq1rk1/ppp2ppp/2nbpn2/3p4/3P4/2PBPN2/PP3PPP/RNBQ1RK1 w - - 0 8",
        "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "rnbqkb1r/pp3ppp/4pn2/2pp4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w KQkq - 0 5",
        "r2q1rk1/pp2bppp/2n1pn2/3p4/3P4/2NBPN2/PP3PPP/R2Q1RK1 b - - 3 10",
        "2r2rk1/pp1bqpp1/2n1pn1p/3p4/3P4/P1NBPN2/1P2QPPP/2R2RK1 w - - 2 15",
        "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/5N1P/PPBN1PP1/R1BQR1K1 w - - 1 13",
        "8/8/1p1k4/p1p1p3/P1P1P3/1P1K4/8/8 w - - 0 40",
        "6k1/5p2/6p1/8/7p/8/6PP/6K1 b - - 0 45",
        "4r1k1/pp3ppp/2p5/8/3P4/2P5/PP3PPP/4R1K1 w - - 0 30",
        "8/5pk1/6p1/1R5p/7P/6P1/r4PK1/8 b - - 5 50",
        "2kr3r/ppp2ppp/2n5/2b1q3/4P3/2N1B3/PPP2PPP/R2QK2R w KQ - 0 12",
        "r4rk1/ppqb1ppp/4pn2/3p4/1b1P4/2NBPN2/PPQ2PPP/R4RK1 w - - 4 12",
        "8/8/4k3/8/2p5/2P5/3K4/8 w - - 0 60",
        "7k/8/5KQ1/8/8/8/8/8 w - - 0 1",
    };
    return fens;
}

const std::vector<TacticPosition>& tactic_positions()
{
    static const std::vector<TacticPosition> positions{
        {"mate-in-one", "7k/8/5KQ1/8/8/8/8/8 w - - 0 1", {"g6g7"}},
        {"win-undefended-rook", "4k3/8/8/8/8/8/4r3/4Q1K1 w - - 0 1", {"e1e2"}},
    };
    return positions;
}

} // namespace

int run_perft_bench_command(std::ostream& output)
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
        output << "perftbench depth " << position.depth << " nodes " << nodes << '\n';
    }

    const auto total_ms = elapsed_ms(start);
    output << "perftbench total nodes " << total_nodes
           << " time " << total_ms
           << " nps " << (total_nodes * 1000 / total_ms) << '\n';
    return 0;
}

int run_bench_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    constexpr int default_bench_depth = 10;
    const int depth = parse_positive_int(args, 1).value_or(default_bench_depth);
    if (args.size() > 1 && !parse_positive_int(args, 1)) {
        error << "usage: aetherz bench [depth]\n";
        return 1;
    }

    // Bench what the engine plays with: the built-in network, if there is one.
    Search search;
    if (!nnue::embedded_network().empty()) {
        (void)search.load_nnue(nnue::embedded_network_name);
    }
    std::uint64_t total_nodes = 0;
    const auto start = std::chrono::steady_clock::now();

    for (const std::string_view fen : bench_fens()) {
        auto board = Board::from_fen(fen);
        if (!board) {
            error << "invalid bench FEN: " << fen << '\n';
            return 1;
        }

        SearchLimits limits;
        limits.max_depth = depth;
        limits.position_history = {board->hash()};

        search.clear_hash();
        const SearchResult result = search.find_best_move(*board, limits);
        total_nodes += result.nodes;
    }

    const auto total_ms = elapsed_ms(start);
    output << "bench depth " << depth
           << " positions " << bench_fens().size()
           << " time " << total_ms << '\n';
    output << total_nodes << " nodes " << (total_nodes * 1000 / total_ms) << " nps\n";
    return 0;
}

int run_search_bench_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    const int depth = parse_positive_int(args, 1).value_or(2);
    const int hash_mb = parse_positive_int(args, 2).value_or(32);

    if ((args.size() > 1 && !parse_positive_int(args, 1)) || (args.size() > 2 && !parse_positive_int(args, 2))) {
        error << "usage: aetherz searchbench [depth] [hash-mb]\n";
        return 1;
    }

    Search search;
    search.set_hash_size_mb(static_cast<std::size_t>(std::clamp(hash_mb, 1, 1024)));

    std::uint64_t total_nodes = 0;
    std::int64_t total_ms = 0;

    output << "searchbench depth " << depth << " hash " << std::clamp(hash_mb, 1, 1024) << '\n';

    for (const auto& position : search_positions()) {
        auto board = Board::from_fen(position.fen);
        if (!board) {
            error << "invalid benchmark FEN: " << position.name << '\n';
            return 1;
        }

        SearchLimits limits;
        limits.max_depth = depth;
        limits.position_history = {board->hash()};

        search.clear_hash();
        const auto start = std::chrono::steady_clock::now();
        const SearchResult result = search.find_best_move(*board, limits);
        const auto ms = elapsed_ms(start);

        total_nodes += result.nodes;
        total_ms += ms;

        output << "position " << position.name
               << " depth " << result.depth
               << " score " << result.score
               << " nodes " << result.nodes
               << " time " << ms
               << " nps " << (result.nodes * 1000 / ms)
               << " best " << (result.best_move ? move_to_uci(*result.best_move) : "0000");

        if (!result.principal_variation.empty()) {
            output << " pv " << pv_to_string(result.principal_variation);
        }
        output << '\n';
    }

    total_ms = std::max<std::int64_t>(1, total_ms);
    output << "searchbench summary positions " << search_positions().size()
           << " nodes " << total_nodes
           << " time " << total_ms
           << " nps " << (total_nodes * 1000 / total_ms) << '\n';

    return 0;
}

int run_tactics_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    const int depth = parse_positive_int(args, 1).value_or(3);
    if (args.size() > 1 && !parse_positive_int(args, 1)) {
        error << "usage: aetherz tactics [depth]\n";
        return 1;
    }

    int passed = 0;
    int failed = 0;

    for (const auto& position : tactic_positions()) {
        auto board = Board::from_fen(position.fen);
        if (!board) {
            error << "invalid tactic FEN: " << position.name << '\n';
            return 1;
        }

        Search search;
        SearchLimits limits;
        limits.max_depth = depth;
        limits.position_history = {board->hash()};

        const auto start = std::chrono::steady_clock::now();
        const SearchResult result = search.find_best_move(*board, limits);
        const auto ms = elapsed_ms(start);

        const bool ok = result.best_move && contains_move(position.best_moves, *result.best_move);
        passed += ok ? 1 : 0;
        failed += ok ? 0 : 1;

        output << "tactic " << (ok ? "ok " : "fail ") << position.name
               << " depth " << result.depth
               << " best " << (result.best_move ? move_to_uci(*result.best_move) : "0000")
               << " score " << result.score
               << " nodes " << result.nodes
               << " time " << ms << '\n';
    }

    output << "tactics summary passed " << passed << " failed " << failed << '\n';
    return failed == 0 ? 0 : 1;
}

int run_selfplay_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    const int games = parse_positive_int(args, 1).value_or(1);
    const int depth = parse_positive_int(args, 2).value_or(2);
    const int max_plies = parse_positive_int(args, 3).value_or(80);

    if ((args.size() > 1 && !parse_positive_int(args, 1))
        || (args.size() > 2 && !parse_positive_int(args, 2))
        || (args.size() > 3 && !parse_positive_int(args, 3))) {
        error << "usage: aetherz selfplay [games] [depth] [max-plies]\n";
        return 1;
    }

    int white_wins = 0;
    int black_wins = 0;
    int draws = 0;

    output << "selfplay games " << games << " depth " << depth << " maxplies " << max_plies << '\n';

    for (int game = 1; game <= games; ++game) {
        Board board = Board::startpos();
        Search white_search;
        Search black_search;
        std::vector<std::uint64_t> history{board.hash()};
        std::vector<Move> moves;
        std::string result = "*";
        std::string_view reason = "max-plies";

        for (int ply = 0; ply < max_plies; ++ply) {
            if (board.generate_legal_moves().empty()) {
                result = result_for_terminal_board(board);
                reason = board.in_check(board.side_to_move()) ? "checkmate" : "stalemate";
                break;
            }
            if (board.halfmove_clock() >= 100) {
                result = "1/2-1/2";
                reason = "fifty-move";
                break;
            }
            if (repetition_count(history, board.hash()) >= 3) {
                result = "1/2-1/2";
                reason = "repetition";
                break;
            }

            Move played;
            Search& search = board.side_to_move() == Color::White ? white_search : black_search;
            if (!play_best_move(board, search, depth, history, played)) {
                result = result_for_terminal_board(board);
                reason = "no-bestmove";
                break;
            }

            moves.push_back(played);
            history.push_back(board.hash());
        }

        if (result == "*") {
            result = "1/2-1/2";
        }

        if (result == "1-0") {
            ++white_wins;
        } else if (result == "0-1") {
            ++black_wins;
        } else {
            ++draws;
        }

        output << "game " << game
               << " result " << result
               << " reason " << reason
               << " plies " << moves.size()
               << " moves";
        for (const Move& move : moves) {
            output << ' ' << move_to_uci(move);
        }
        output << '\n';
    }

    output << "selfplay summary white " << white_wins
           << " black " << black_wins
           << " draw " << draws << '\n';
    return 0;
}

int run_epd_test_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    if (args.size() < 2 || args.size() > 4) {
        error << "usage: aetherz epdtest <file> [depth] [max-positions]\n";
        return 1;
    }

    const int depth = parse_positive_int(args, 2).value_or(3);
    const int max_positions = parse_positive_int(args, 3).value_or(0);
    if ((args.size() > 2 && !parse_positive_int(args, 2)) || (args.size() > 3 && !parse_positive_int(args, 3))) {
        error << "usage: aetherz epdtest <file> [depth] [max-positions]\n";
        return 1;
    }

    const auto positions = load_epd_file(args[1]);
    if (positions.empty()) {
        error << "no EPD positions loaded from " << args[1] << '\n';
        return 1;
    }

    int tested = 0;
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    output << "epdtest file " << args[1] << " depth " << depth << '\n';

    for (const EpdPosition& position : positions) {
        if (max_positions > 0 && tested >= max_positions) {
            break;
        }

        if (position.best_moves.empty() && position.avoid_moves.empty()) {
            ++skipped;
            continue;
        }

        ++tested;

        auto board = Board::from_fen(position.fen);
        if (!board) {
            ++failed;
            output << "epd fail " << (position.id.empty() ? "<no-id>" : position.id)
                   << " invalid-fen\n";
            continue;
        }

        Search search;
        SearchLimits limits;
        limits.max_depth = depth;
        limits.position_history = {board->hash()};

        const auto start = std::chrono::steady_clock::now();
        const SearchResult result = search.find_best_move(*board, limits);
        const auto ms = elapsed_ms(start);
        const std::string best = result.best_move ? move_to_uci(*result.best_move) : "0000";

        const bool matched_best = result.best_move && contains_epd_move(*board, position.best_moves, *result.best_move);
        const bool matched_avoid = result.best_move && contains_epd_move(*board, position.avoid_moves, *result.best_move);
        const bool ok = result.best_move.has_value() && (!position.best_moves.empty() ? matched_best : !matched_avoid);

        passed += ok ? 1 : 0;
        failed += ok ? 0 : 1;

        output << "epd " << (ok ? "ok " : "fail ")
               << (position.id.empty() ? "<no-id>" : position.id)
               << " best " << best
               << " score " << result.score
               << " nodes " << result.nodes
               << " time " << ms;
        if (!position.best_moves.empty()) {
            output << " bm " << join_words(position.best_moves);
        }
        if (!position.avoid_moves.empty()) {
            output << " am " << join_words(position.avoid_moves);
        }
        output << '\n';
    }

    output << "epdtest summary tested " << tested
           << " passed " << passed
           << " failed " << failed
           << " skipped " << skipped << '\n';
    return failed == 0 ? 0 : 1;
}

int run_arasan_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    std::vector<std::string> epd_args{"epdtest", "testsuites/arasan2024.epd"};
    if (args.size() > 1) {
        epd_args.push_back(args[1]);
    }
    if (args.size() > 2) {
        epd_args.push_back(args[2]);
    }
    if (args.size() > 3) {
        error << "usage: aetherz arasan [depth] [max-positions]\n";
        return 1;
    }
    return run_epd_test_command(epd_args, output, error);
}

} // namespace aetherz
