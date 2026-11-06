#include "engine/datagen.hpp"

#include "engine/board.hpp"
#include "engine/search.hpp"
#include "engine/thread.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace aetherz {
namespace {

constexpr int max_game_plies = 400;
constexpr int win_adjudication_score = 2000;
constexpr int win_adjudication_plies = 6;
constexpr int draw_adjudication_score = 10;
constexpr int draw_adjudication_plies = 12;
constexpr int draw_adjudication_min_ply = 80;
constexpr int unbalanced_opening_score = 1000;

bool is_tactical(const Move& move)
{
    return move.flag == MoveFlag::Capture
        || move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::Promotion
        || move.flag == MoveFlag::PromotionCapture;
}

bool insufficient_material(const Board& board)
{
    const auto count = [&](Piece piece) {
        return std::popcount(board.pieces(piece));
    };

    if (count(Piece::WhitePawn) + count(Piece::BlackPawn) + count(Piece::WhiteRook) + count(Piece::BlackRook)
            + count(Piece::WhiteQueen) + count(Piece::BlackQueen)
        != 0) {
        return false;
    }

    return count(Piece::WhiteKnight) + count(Piece::BlackKnight) + count(Piece::WhiteBishop)
        + count(Piece::BlackBishop)
        <= 1;
}

struct Sample {
    std::string fen;
    int white_score = 0;
};

enum class GameResult {
    WhiteWins,
    BlackWins,
    Draw,
};

const char* result_text(GameResult result)
{
    switch (result) {
    case GameResult::WhiteWins:
        return "1.0";
    case GameResult::BlackWins:
        return "0.0";
    case GameResult::Draw:
        return "0.5";
    }
    return "0.5";
}

// Plays random legal moves from the start position. Returns false when the
// game ends during the random moves.
bool play_random_opening(Board& board, std::vector<std::uint64_t>& history, int plies, std::mt19937_64& rng)
{
    board = Board::startpos();
    history = {board.hash()};
    for (int ply = 0; ply < plies; ++ply) {
        const MoveList moves = board.generate_legal_moves();
        if (moves.empty()) {
            return false;
        }
        std::uniform_int_distribution<std::size_t> pick{0, moves.size() - 1};
        Board::UndoState undo;
        if (!board.make_move(moves[pick(rng)], undo)) {
            return false;
        }
        history.push_back(board.hash());
    }
    return !board.generate_legal_moves().empty();
}

// Plays one self-play game and returns its training samples and result.
GameResult play_game(
    Search& search,
    const DatagenConfig& config,
    std::uint64_t game_index,
    std::vector<Sample>& samples)
{
    // Every game depends only on the seed and its index.
    std::mt19937_64 rng{config.seed ^ (0x9E3779B97F4A7C15ULL * (game_index + 1))};
    const int random_plies = config.random_plies + static_cast<int>(rng() & 1);

    Board board;
    std::vector<std::uint64_t> history;
    while (true) {
        samples.clear();
        search.clear();
        if (!play_random_opening(board, history, random_plies, rng)) {
            continue;
        }

        int win_streak = 0;
        Color win_side = Color::White;
        int draw_streak = 0;
        bool unbalanced_opening = false;

        for (int ply = 0; ply < max_game_plies; ++ply) {
            const MoveList legal_moves = board.generate_legal_moves();
            const bool in_check = board.in_check(board.side_to_move());
            if (legal_moves.empty()) {
                if (!in_check) {
                    return GameResult::Draw;
                }
                return board.side_to_move() == Color::White ? GameResult::BlackWins : GameResult::WhiteWins;
            }
            if (board.halfmove_clock() >= 100 || insufficient_material(board)
                || std::count(history.begin(), history.end(), board.hash()) >= 3) {
                return GameResult::Draw;
            }

            SearchLimits limits;
            limits.max_depth = max_search_depth;
            limits.max_nodes = config.nodes;
            limits.position_history = history;
            Board search_board = board;
            const SearchResult result = search.find_best_move(search_board, limits);
            if (!result.best_move) {
                return GameResult::Draw;
            }

            const int white_score = board.side_to_move() == Color::White ? result.score : -result.score;
            if (ply == 0 && std::abs(white_score) > unbalanced_opening_score) {
                unbalanced_opening = true;
                break;
            }

            if (!in_check && !is_tactical(*result.best_move) && std::abs(white_score) < mate_bound) {
                samples.push_back(Sample{.fen = board.to_fen(), .white_score = white_score});
            }

            if (std::abs(white_score) >= win_adjudication_score) {
                const Color leader = white_score > 0 ? Color::White : Color::Black;
                win_streak = win_streak > 0 && leader == win_side ? win_streak + 1 : 1;
                win_side = leader;
                if (win_streak >= win_adjudication_plies) {
                    return leader == Color::White ? GameResult::WhiteWins : GameResult::BlackWins;
                }
            } else {
                win_streak = 0;
            }

            draw_streak = std::abs(white_score) <= draw_adjudication_score ? draw_streak + 1 : 0;
            if (ply >= draw_adjudication_min_ply && draw_streak >= draw_adjudication_plies) {
                return GameResult::Draw;
            }

            Board::UndoState undo;
            if (!board.make_move(*result.best_move, undo)) {
                return GameResult::Draw;
            }
            history.push_back(board.hash());
        }

        if (!unbalanced_opening) {
            return GameResult::Draw;
        }
    }
}

std::optional<std::uint64_t> parse_unsigned(const std::string& text)
{
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used);
        if (used == text.size()) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

} // namespace

DatagenStats generate_training_data(const DatagenConfig& config, std::ostream* progress, std::string* error)
{
    DatagenStats stats;
    // Checked once up front; every worker then loads its own copy.
    if (!config.eval_file.empty()) {
        std::string load_error;
        nnue::NnueEvaluator probe;
        if (!probe.load(config.eval_file, &load_error)) {
            if (error != nullptr) {
                *error = "could not load NNUE: " + load_error;
            }
            return stats;
        }
    }

    std::ofstream output{config.output_path, std::ios::app};
    if (!output) {
        if (error != nullptr) {
            *error = "could not open " + config.output_path + " for writing";
        }
        return stats;
    }
    stats.ok = true;

    std::mutex mutex;
    std::atomic<std::uint64_t> next_game{0};
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t report_every = std::max<std::uint64_t>(1, config.games / 100);

    auto worker = [&]() {
        auto search = std::make_unique<Search>();
        search->set_hash_size_mb(static_cast<std::size_t>(std::max(1, config.hash_mb)));
        if (!config.eval_file.empty()) {
            (void)search->load_nnue(config.eval_file);
        }
        std::vector<Sample> samples;
        std::string lines;

        while (true) {
            const std::uint64_t game = next_game++;
            if (game >= config.games) {
                break;
            }

            const GameResult result = play_game(*search, config, game, samples);
            lines.clear();
            for (const Sample& sample : samples) {
                lines += sample.fen;
                lines += " | ";
                lines += std::to_string(sample.white_score);
                lines += " | ";
                lines += result_text(result);
                lines += '\n';
            }

            std::lock_guard lock{mutex};
            output << lines;
            ++stats.games;
            stats.positions += samples.size();
            stats.white_wins += result == GameResult::WhiteWins ? 1 : 0;
            stats.black_wins += result == GameResult::BlackWins ? 1 : 0;
            stats.draws += result == GameResult::Draw ? 1 : 0;

            if (progress != nullptr && (stats.games % report_every == 0 || stats.games == config.games)) {
                const double seconds = std::max(
                    1e-3, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                *progress << "datagen games " << stats.games << '/' << config.games
                          << " positions " << stats.positions
                          << " (" << static_cast<std::uint64_t>(static_cast<double>(stats.positions) / seconds) << "/s)"
                          << " white " << stats.white_wins << " black " << stats.black_wins
                          << " draw " << stats.draws << std::endl;
            }
        }
    };

    std::vector<SearchThread> threads;
    for (int index = 0; index < std::max(1, config.threads); ++index) {
        threads.emplace_back(worker);
    }
    for (SearchThread& thread : threads) {
        thread.join();
    }
    return stats;
}

int run_datagen_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error)
{
    const auto usage = [&error] {
        error << "usage: aetherz datagen <output-file> [--games N] [--threads N] [--nodes N]\n"
                 "                       [--random-plies N] [--seed N] [--hash MB]\n"
                 "                       [--eval-file <network or <embedded>>]\n";
        return 1;
    };

    if (args.size() < 2) {
        return usage();
    }

    DatagenConfig config;
    config.output_path = args[1];
    for (std::size_t index = 2; index < args.size(); index += 2) {
        if (index + 1 >= args.size()) {
            return usage();
        }
        if (args[index] == "--eval-file") {
            config.eval_file = args[index + 1];
            continue;
        }
        const auto value = parse_unsigned(args[index + 1]);
        if (!value) {
            return usage();
        }

        const std::string& name = args[index];
        if (name == "--games") {
            config.games = *value;
        } else if (name == "--threads") {
            config.threads = static_cast<int>(std::clamp<std::uint64_t>(*value, 1, 1024));
        } else if (name == "--nodes") {
            config.nodes = std::max<std::uint64_t>(1, *value);
        } else if (name == "--random-plies") {
            config.random_plies = static_cast<int>(std::min<std::uint64_t>(*value, 40));
        } else if (name == "--seed") {
            config.seed = *value;
        } else if (name == "--hash") {
            config.hash_mb = static_cast<int>(std::clamp<std::uint64_t>(*value, 1, 4096));
        } else {
            return usage();
        }
    }

    output << "datagen " << config.games << " games, " << config.threads << " threads, "
           << config.nodes << " nodes per move, " << config.random_plies << "-" << config.random_plies + 1
           << " random plies, seed " << config.seed << ", eval "
           << (config.eval_file.empty() ? std::string{"classical"} : config.eval_file)
           << ", appending to " << config.output_path << std::endl;
    std::string failure;
    const DatagenStats stats = generate_training_data(config, &output, &failure);
    if (!stats.ok) {
        error << failure << '\n';
        return 1;
    }
    output << "datagen wrote " << stats.positions << " positions from " << stats.games << " games" << std::endl;
    return 0;
}

} // namespace aetherz
