// aetherz-match: plays two UCI engines against each other and runs an SPRT.
//
// Games are played in pairs from the same opening with colors swapped, so
// results are tracked as a pentanomial distribution over game pairs.

#include "engine/board.hpp"
#include "engine/search.hpp"
#include "engine/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <poll.h>
#include <random>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;
using aetherz::Board;
using aetherz::Color;
using aetherz::Move;
using aetherz::PieceType;

struct EngineConfig {
    std::string name;
    std::string command;
    std::vector<std::pair<std::string, std::string>> options;
};

struct MatchConfig {
    std::array<EngineConfig, 2> engines;
    int pairs = 1000;
    int concurrency = 1;
    double base_seconds = 8.0;
    double increment_seconds = 0.08;
    int time_margin_ms = 100;
    std::optional<std::string> openings_file;
    int random_opening_plies = 8;
    std::uint64_t seed = 0x4145544845525aULL;
    bool sprt = false;
    double elo0 = 0.0;
    double elo1 = 5.0;
    double alpha = 0.05;
    double beta = 0.05;
    std::optional<std::string> games_file;
    std::optional<std::string> save_openings_file;
    int report_every = 10;
};

class EngineProcess {
public:
    EngineProcess() = default;
    EngineProcess(const EngineProcess&) = delete;
    EngineProcess& operator=(const EngineProcess&) = delete;

    ~EngineProcess()
    {
        stop();
    }

    bool start(const EngineConfig& config)
    {
        stop();

        int to_child[2];
        int from_child[2];
        if (pipe(to_child) != 0) {
            return false;
        }
        if (pipe(from_child) != 0) {
            close(to_child[0]);
            close(to_child[1]);
            return false;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, to_child[1]);
        posix_spawn_file_actions_addclose(&actions, from_child[0]);

        std::string command = config.command;
        char* argv[] = {command.data(), nullptr};
        const int status = posix_spawn(&pid_, command.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(to_child[0]);
        close(from_child[1]);

        if (status != 0) {
            close(to_child[1]);
            close(from_child[0]);
            pid_ = -1;
            return false;
        }

        input_fd_ = to_child[1];
        output_fd_ = from_child[0];
        buffer_.clear();

        send("uci");
        if (!wait_for("uciok", std::chrono::seconds{10})) {
            stop();
            return false;
        }
        for (const auto& [name, value] : config.options) {
            send("setoption name " + name + " value " + value);
        }
        return ready();
    }

    bool ready()
    {
        send("isready");
        return wait_for("readyok", std::chrono::seconds{30});
    }

    [[nodiscard]] bool running() const
    {
        return pid_ > 0;
    }

    void send(const std::string& line)
    {
        if (input_fd_ < 0) {
            return;
        }

        const std::string message = line + '\n';
        std::size_t written = 0;
        while (written < message.size()) {
            const ssize_t result = write(input_fd_, message.data() + written, message.size() - written);
            if (result <= 0) {
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
            written += static_cast<std::size_t>(result);
        }
    }

    // Returns nullopt on timeout or when the engine closed its output.
    std::optional<std::string> read_line(Clock::time_point deadline)
    {
        while (true) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }

            if (output_fd_ < 0) {
                return std::nullopt;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) {
                return std::nullopt;
            }

            pollfd descriptor{.fd = output_fd_, .events = POLLIN, .revents = 0};
            const int polled = poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 1000)));
            if (polled < 0 && errno != EINTR) {
                return std::nullopt;
            }
            if (polled <= 0) {
                continue;
            }

            char chunk[4096];
            const ssize_t count = read(output_fd_, chunk, sizeof(chunk));
            if (count <= 0) {
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                close(output_fd_);
                output_fd_ = -1;
                return std::nullopt;
            }
            buffer_.append(chunk, static_cast<std::size_t>(count));
        }
    }

    void stop()
    {
        if (pid_ <= 0) {
            return;
        }

        send("quit");
        if (input_fd_ >= 0) {
            close(input_fd_);
            input_fd_ = -1;
        }

        const auto deadline = Clock::now() + std::chrono::seconds{2};
        int status = 0;
        while (waitpid(pid_, &status, WNOHANG) == 0) {
            if (Clock::now() >= deadline) {
                kill(pid_, SIGKILL);
                waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        if (output_fd_ >= 0) {
            close(output_fd_);
            output_fd_ = -1;
        }
        pid_ = -1;
    }

private:
    pid_t pid_ = -1;
    int input_fd_ = -1;
    int output_fd_ = -1;
    std::string buffer_;

    bool wait_for(std::string_view token, std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (const auto line = read_line(deadline)) {
            if (*line == token) {
                return true;
            }
        }
        return false;
    }
};

enum class Outcome {
    WhiteWins,
    BlackWins,
    Draw,
};

struct GameRecord {
    Outcome outcome = Outcome::Draw;
    std::string reason;
    std::string start_fen;
    std::vector<std::string> moves;
};

bool insufficient_material(const Board& board)
{
    using aetherz::Piece;
    const auto count = [&](Piece piece) {
        return std::popcount(board.pieces(piece));
    };

    if (count(Piece::WhitePawn) + count(Piece::BlackPawn) + count(Piece::WhiteRook) + count(Piece::BlackRook)
            + count(Piece::WhiteQueen) + count(Piece::BlackQueen)
        != 0) {
        return false;
    }

    const int minors = count(Piece::WhiteKnight) + count(Piece::BlackKnight) + count(Piece::WhiteBishop)
        + count(Piece::BlackBishop);
    return minors <= 1;
}

std::optional<Move> find_legal_move(const Board& board, std::string_view text)
{
    const auto parsed = aetherz::move_from_uci(text);
    if (!parsed) {
        return std::nullopt;
    }

    for (const Move& move : board.generate_legal_moves()) {
        if (move.from == parsed->from && move.to == parsed->to && move.promotion == parsed->promotion) {
            return move;
        }
    }
    return std::nullopt;
}

// Score from the engine's own point of view, with mates mapped far outside
// the centipawn range.
std::optional<int> parse_score(const std::string& line)
{
    std::istringstream stream{line};
    std::string token;
    while (stream >> token) {
        if (token != "score") {
            continue;
        }

        std::string kind;
        int value = 0;
        if (!(stream >> kind >> value)) {
            return std::nullopt;
        }
        if (kind == "cp") {
            return value;
        }
        if (kind == "mate") {
            return value > 0 ? 100'000 - value : -100'000 - value;
        }
    }
    return std::nullopt;
}

Outcome win_for(Color color)
{
    return color == Color::White ? Outcome::WhiteWins : Outcome::BlackWins;
}

GameRecord play_game(
    const MatchConfig& config,
    std::array<EngineProcess*, 2> players, // indexed by color
    const std::array<const EngineConfig*, 2>& player_configs,
    const std::string& start_fen)
{
    constexpr int resign_score = 1000;
    constexpr int resign_plies = 6;
    constexpr int draw_score = 10;
    constexpr int draw_plies = 12;
    constexpr int draw_min_ply = 80;
    constexpr int max_plies = 500;

    GameRecord record;
    record.start_fen = start_fen;

    auto parsed = Board::from_fen(start_fen);
    if (!parsed) {
        record.reason = "invalid opening";
        return record;
    }
    Board board = *parsed;
    std::vector<std::uint64_t> history{board.hash()};

    const auto base_ms = static_cast<std::int64_t>(config.base_seconds * 1000.0);
    const auto increment_ms = static_cast<std::int64_t>(config.increment_seconds * 1000.0);
    std::array<std::int64_t, 2> remaining_ms{base_ms, base_ms};

    for (EngineProcess* engine : players) {
        engine->send("ucinewgame");
        engine->ready();
    }

    int resign_streak = 0;
    Color resign_winner = Color::White;
    int draw_streak = 0;

    for (int ply = 0; ply < max_plies; ++ply) {
        const auto legal_moves = board.generate_legal_moves();
        if (legal_moves.empty()) {
            if (board.in_check(board.side_to_move())) {
                record.outcome = win_for(aetherz::opposite(board.side_to_move()));
                record.reason = "checkmate";
            } else {
                record.outcome = Outcome::Draw;
                record.reason = "stalemate";
            }
            return record;
        }
        if (board.halfmove_clock() >= 100) {
            record.reason = "fifty moves";
            return record;
        }
        if (std::count(history.begin(), history.end(), board.hash()) >= 3) {
            record.reason = "repetition";
            return record;
        }
        if (insufficient_material(board)) {
            record.reason = "insufficient material";
            return record;
        }

        const Color mover = board.side_to_move();
        const int mover_index = aetherz::color_index(mover);
        EngineProcess& engine = *players[mover_index];

        std::string position = "position fen " + start_fen;
        if (!record.moves.empty()) {
            position += " moves";
            for (const std::string& move : record.moves) {
                position += ' ';
                position += move;
            }
        }
        engine.send(position);

        std::ostringstream go;
        go << "go wtime " << remaining_ms[0] << " btime " << remaining_ms[1]
           << " winc " << increment_ms << " binc " << increment_ms;
        const auto started = Clock::now();
        engine.send(go.str());

        const auto deadline = started + std::chrono::milliseconds{remaining_ms[mover_index] + config.time_margin_ms + 1000};
        std::optional<int> score;
        std::string best_move;
        while (const auto line = engine.read_line(deadline)) {
            if (line->starts_with("info")) {
                if (const auto parsed_score = parse_score(*line)) {
                    score = parsed_score;
                }
            } else if (line->starts_with("bestmove")) {
                std::istringstream stream{*line};
                std::string keyword;
                stream >> keyword >> best_move;
                break;
            }
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (best_move.empty()) {
            record.outcome = win_for(aetherz::opposite(mover));
            record.reason = player_configs[mover_index]->name + " disconnected or hung";
            engine.stop();
            return record;
        }
        if (elapsed_ms > remaining_ms[mover_index] + config.time_margin_ms) {
            record.outcome = win_for(aetherz::opposite(mover));
            record.reason = player_configs[mover_index]->name + " lost on time";
            return record;
        }
        remaining_ms[mover_index] += increment_ms - elapsed_ms;

        const auto move = find_legal_move(board, best_move);
        if (!move) {
            record.outcome = win_for(aetherz::opposite(mover));
            record.reason = player_configs[mover_index]->name + " played illegal move " + best_move;
            return record;
        }

        Board::UndoState undo;
        if (!board.make_move(*move, undo)) {
            record.outcome = win_for(aetherz::opposite(mover));
            record.reason = "internal move error";
            return record;
        }
        record.moves.push_back(aetherz::move_to_uci(*move));
        history.push_back(board.hash());

        if (score) {
            if (std::abs(*score) >= resign_score) {
                const Color winner = *score > 0 ? mover : aetherz::opposite(mover);
                resign_streak = resign_streak > 0 && winner == resign_winner ? resign_streak + 1 : 1;
                resign_winner = winner;
            } else {
                resign_streak = 0;
            }

            draw_streak = std::abs(*score) <= draw_score ? draw_streak + 1 : 0;
        } else {
            resign_streak = 0;
            draw_streak = 0;
        }

        if (resign_streak >= resign_plies) {
            record.outcome = win_for(resign_winner);
            record.reason = "adjudicated win";
            return record;
        }
        if (ply >= draw_min_ply && draw_streak >= draw_plies) {
            record.reason = "adjudicated draw";
            return record;
        }
    }

    record.reason = "max plies";
    return record;
}

std::vector<std::string> load_openings(const std::string& path)
{
    std::vector<std::string> openings;
    std::ifstream input{path};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream{line};
        std::vector<std::string> fields;
        std::string field;
        while (fields.size() < 6 && stream >> field) {
            fields.push_back(field);
        }
        if (fields.size() < 4) {
            continue;
        }

        std::string fen = fields[0] + ' ' + fields[1] + ' ' + fields[2] + ' ' + fields[3];
        const bool has_counters = fields.size() == 6
            && std::ranges::all_of(fields[4], [](unsigned char value) { return std::isdigit(value) != 0; })
            && std::ranges::all_of(fields[5], [](unsigned char value) { return std::isdigit(value) != 0; });
        fen += has_counters ? ' ' + fields[4] + ' ' + fields[5] : " 0 1";
        if (Board::from_fen(fen)) {
            openings.push_back(fen);
        }
    }
    return openings;
}

// Random openings: a few random legal moves from the start position, kept
// only if a short search considers the result roughly balanced.
std::vector<std::string> generate_openings(int count, int plies, std::uint64_t seed)
{
    constexpr int max_balance_cp = 120;
    constexpr int check_depth = 6;

    std::vector<std::string> openings;
    std::mt19937_64 rng{seed};
    aetherz::Search search;

    while (static_cast<int>(openings.size()) < count) {
        Board board = Board::startpos();
        bool ok = true;
        for (int ply = 0; ply < plies; ++ply) {
            const auto moves = board.generate_legal_moves();
            if (moves.empty()) {
                ok = false;
                break;
            }
            std::uniform_int_distribution<std::size_t> pick{0, moves.size() - 1};
            Board::UndoState undo;
            if (!board.make_move(moves[pick(rng)], undo)) {
                ok = false;
                break;
            }
        }
        if (!ok || board.generate_legal_moves().empty()) {
            continue;
        }

        aetherz::SearchLimits limits;
        limits.max_depth = check_depth;
        limits.position_history = {board.hash()};
        Board search_board = board;
        search.clear_hash();
        const auto result = search.find_best_move(search_board, limits);
        if (std::abs(result.score) <= max_balance_cp) {
            openings.push_back(board.to_fen());
        }
    }
    return openings;
}

struct Statistics {
    // Pair outcomes from engine 1's point of view: 0, 0.5, 1, 1.5, 2 points.
    std::array<int, 5> pentanomial{};
    int wins = 0;
    int losses = 0;
    int draws = 0;

    [[nodiscard]] int pairs() const
    {
        int total = 0;
        for (const int count : pentanomial) {
            total += count;
        }
        return total;
    }
};

double expected_score(double elo)
{
    return 1.0 / (1.0 + std::pow(10.0, -elo / 400.0));
}

double elo_from_score(double score)
{
    score = std::clamp(score, 1e-6, 1.0 - 1e-6);
    return -400.0 * std::log10(1.0 / score - 1.0);
}

struct PairSummary {
    double mean = 0.5;
    double variance = 0.0;
};

PairSummary summarize(const Statistics& stats)
{
    PairSummary summary;
    const int pairs = stats.pairs();
    if (pairs == 0) {
        return summary;
    }

    // Regularize empty buckets slightly so early variance estimates stay sane.
    std::array<double, 5> probabilities{};
    double total = 0.0;
    for (int index = 0; index < 5; ++index) {
        probabilities[index] = stats.pentanomial[index] + 1e-3;
        total += probabilities[index];
    }

    summary.mean = 0.0;
    for (int index = 0; index < 5; ++index) {
        probabilities[index] /= total;
        summary.mean += probabilities[index] * index / 4.0;
    }
    for (int index = 0; index < 5; ++index) {
        const double delta = index / 4.0 - summary.mean;
        summary.variance += probabilities[index] * delta * delta;
    }
    return summary;
}

// The variance estimate is meaningless on a handful of pairs (two identical
// pair results give a variance near zero and an enormous LLR), so the SPRT
// may not conclude before this many pairs.
constexpr int min_sprt_pairs = 50;

double log_likelihood_ratio(const Statistics& stats, double elo0, double elo1)
{
    const int pairs = stats.pairs();
    if (pairs < min_sprt_pairs) {
        return 0.0;
    }

    const PairSummary summary = summarize(stats);
    if (summary.variance <= 0.0) {
        return 0.0;
    }

    const double score0 = expected_score(elo0);
    const double score1 = expected_score(elo1);
    return pairs * (score1 - score0) * (2.0 * summary.mean - score0 - score1) / (2.0 * summary.variance);
}

void print_status(std::ostream& output, const MatchConfig& config, const Statistics& stats)
{
    const int pairs = stats.pairs();
    const PairSummary summary = summarize(stats);
    const double elo = elo_from_score(summary.mean);
    const double error = pairs > 1 ? 1.96 * std::sqrt(summary.variance / pairs) : 0.5;
    const double elo_low = elo_from_score(summary.mean - error);
    const double elo_high = elo_from_score(summary.mean + error);

    output << std::fixed << std::setprecision(2)
           << "games " << pairs * 2
           << " | " << config.engines[0].name << " vs " << config.engines[1].name
           << " W " << stats.wins << " L " << stats.losses << " D " << stats.draws
           << " | elo " << elo << " [" << elo_low << ", " << elo_high << "]"
           << " | penta [" << stats.pentanomial[0] << ' ' << stats.pentanomial[1] << ' '
           << stats.pentanomial[2] << ' ' << stats.pentanomial[3] << ' ' << stats.pentanomial[4] << ']';

    if (config.sprt) {
        const double lower = std::log(config.beta / (1.0 - config.alpha));
        const double upper = std::log((1.0 - config.beta) / config.alpha);
        output << " | LLR " << log_likelihood_ratio(stats, config.elo0, config.elo1)
               << " (" << lower << ", " << upper << ") [" << config.elo0 << ", " << config.elo1 << "]";
    }
    output << std::endl;
}

const char* outcome_text(Outcome outcome)
{
    switch (outcome) {
    case Outcome::WhiteWins:
        return "1-0";
    case Outcome::BlackWins:
        return "0-1";
    case Outcome::Draw:
        return "1/2-1/2";
    }
    return "*";
}

void usage()
{
    std::cerr
        << "usage: aetherz-match --engine1 <path> --engine2 <path> [options]\n"
        << "  --name1/--name2 <name>         display names\n"
        << "  --option1/--option2 <n>=<v>    UCI option for one engine (repeatable)\n"
        << "  --option <n>=<v>               UCI option for both engines (repeatable)\n"
        << "  --tc <base>+<inc>              time control in seconds (default 8+0.08)\n"
        << "  --pairs <n>                    maximum game pairs (default 1000)\n"
        << "  --concurrency <n>              games played in parallel (default 1)\n"
        << "  --openings <file>              FEN/EPD openings (default: random openings)\n"
        << "  --random-plies <n>             plies for random openings (default 8)\n"
        << "  --seed <n>                     opening seed\n"
        << "  --sprt <elo0> <elo1>           stop when the SPRT concludes (alpha=beta=0.05)\n"
        << "  --games <file>                 append finished games to a file\n"
        << "  --save-openings <file>         write the generated openings to a file for reuse\n";
}

std::optional<std::pair<std::string, std::string>> parse_option(const std::string& text)
{
    const auto equals = text.find('=');
    if (equals == std::string::npos) {
        return std::nullopt;
    }
    return std::pair{text.substr(0, equals), text.substr(equals + 1)};
}

std::optional<MatchConfig> parse_arguments(int argc, char** argv)
{
    MatchConfig config;
    config.engines[0].name = "engine1";
    config.engines[1].name = "engine2";

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto next = [&]() -> std::string {
                if (index + 1 >= argc) {
                    throw std::invalid_argument{"missing value for " + argument};
                }
                return argv[++index];
            };

            if (argument == "--engine1") {
                config.engines[0].command = next();
            } else if (argument == "--engine2") {
                config.engines[1].command = next();
            } else if (argument == "--name1") {
                config.engines[0].name = next();
            } else if (argument == "--name2") {
                config.engines[1].name = next();
            } else if (argument == "--option1" || argument == "--option2" || argument == "--option") {
                const auto option = parse_option(next());
                if (!option) {
                    throw std::invalid_argument{"options are written as name=value"};
                }
                if (argument != "--option2") {
                    config.engines[0].options.push_back(*option);
                }
                if (argument != "--option1") {
                    config.engines[1].options.push_back(*option);
                }
            } else if (argument == "--tc") {
                const std::string value = next();
                const auto plus = value.find('+');
                config.base_seconds = std::stod(value.substr(0, plus));
                config.increment_seconds = plus == std::string::npos ? 0.0 : std::stod(value.substr(plus + 1));
            } else if (argument == "--pairs") {
                config.pairs = std::stoi(next());
            } else if (argument == "--concurrency") {
                config.concurrency = std::max(1, std::stoi(next()));
            } else if (argument == "--openings") {
                config.openings_file = next();
            } else if (argument == "--random-plies") {
                config.random_opening_plies = std::stoi(next());
            } else if (argument == "--seed") {
                config.seed = std::stoull(next());
            } else if (argument == "--sprt") {
                config.sprt = true;
                config.elo0 = std::stod(next());
                config.elo1 = std::stod(next());
            } else if (argument == "--games") {
                config.games_file = next();
            } else if (argument == "--save-openings") {
                config.save_openings_file = next();
            } else {
                throw std::invalid_argument{"unknown argument " + argument};
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return std::nullopt;
    }

    if (config.engines[0].command.empty() || config.engines[1].command.empty() || config.pairs <= 0) {
        return std::nullopt;
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGPIPE, SIG_IGN);

    const auto parsed = parse_arguments(argc, argv);
    if (!parsed) {
        usage();
        return 1;
    }
    const MatchConfig config = *parsed;

    std::vector<std::string> openings;
    if (config.openings_file) {
        openings = load_openings(*config.openings_file);
        if (openings.empty()) {
            std::cerr << "no openings loaded from " << *config.openings_file << '\n';
            return 1;
        }
    } else {
        std::cout << "generating " << config.pairs << " random openings..." << std::endl;
        openings = generate_openings(config.pairs, config.random_opening_plies, config.seed);
        if (config.save_openings_file) {
            std::ofstream saved{*config.save_openings_file};
            for (const std::string& opening : openings) {
                saved << opening << '\n';
            }
        }
    }

    std::ofstream games_output;
    if (config.games_file) {
        games_output.open(*config.games_file, std::ios::app);
    }

    Statistics stats;
    std::mutex mutex;
    std::atomic<int> next_pair{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};

    auto worker = [&]() {
        std::array<EngineProcess, 2> engines;
        auto ensure_running = [&](int index) {
            if (!engines[index].running() && !engines[index].start(config.engines[index])) {
                std::lock_guard lock{mutex};
                std::cerr << "failed to start " << config.engines[index].command << '\n';
                failed = true;
                finished = true;
                return false;
            }
            return true;
        };

        while (!finished) {
            const int pair = next_pair++;
            if (pair >= config.pairs) {
                break;
            }
            const std::string& opening = openings[static_cast<std::size_t>(pair) % openings.size()];

            // Points scored by engine 1 over both games of the pair, in half points.
            int engine1_half_points = 0;
            std::array<GameRecord, 2> records;
            for (int game = 0; game < 2; ++game) {
                if (!ensure_running(0) || !ensure_running(1)) {
                    return;
                }

                // Game 0: engine 1 is white. Game 1: colors reversed.
                const int white = game == 0 ? 0 : 1;
                const int black = 1 - white;
                records[game] = play_game(
                    config,
                    {&engines[white], &engines[black]},
                    {&config.engines[white], &config.engines[black]},
                    opening);

                const Outcome outcome = records[game].outcome;
                if (outcome == Outcome::Draw) {
                    engine1_half_points += 1;
                } else if ((outcome == Outcome::WhiteWins) == (white == 0)) {
                    engine1_half_points += 2;
                }
            }

            std::lock_guard lock{mutex};
            if (finished) {
                break;
            }
            ++stats.pentanomial[engine1_half_points];
            for (int game = 0; game < 2; ++game) {
                const Outcome outcome = records[game].outcome;
                const bool engine1_white = game == 0;
                if (outcome == Outcome::Draw) {
                    ++stats.draws;
                } else if ((outcome == Outcome::WhiteWins) == engine1_white) {
                    ++stats.wins;
                } else {
                    ++stats.losses;
                }

                if (games_output) {
                    const auto& white_name = config.engines[engine1_white ? 0 : 1].name;
                    const auto& black_name = config.engines[engine1_white ? 1 : 0].name;
                    games_output << outcome_text(outcome) << " | " << white_name << " - " << black_name
                                 << " | " << records[game].reason << " | " << records[game].start_fen << " |";
                    for (const std::string& move : records[game].moves) {
                        games_output << ' ' << move;
                    }
                    games_output << '\n';
                }
            }

            if (stats.pairs() % config.report_every == 0) {
                print_status(std::cout, config, stats);
            }

            if (config.sprt) {
                const double llr = log_likelihood_ratio(stats, config.elo0, config.elo1);
                const double lower = std::log(config.beta / (1.0 - config.alpha));
                const double upper = std::log((1.0 - config.beta) / config.alpha);
                if (llr >= upper || llr <= lower) {
                    finished = true;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int index = 0; index < config.concurrency; ++index) {
        threads.emplace_back(worker);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    if (failed) {
        return 1;
    }

    std::cout << "final: ";
    print_status(std::cout, config, stats);
    if (config.sprt) {
        const double llr = log_likelihood_ratio(stats, config.elo0, config.elo1);
        if (llr >= std::log((1.0 - config.beta) / config.alpha)) {
            std::cout << "SPRT: H1 accepted (" << config.engines[0].name << " is stronger)" << std::endl;
        } else if (llr <= std::log(config.beta / (1.0 - config.alpha))) {
            std::cout << "SPRT: H0 accepted (no improvement)" << std::endl;
        } else {
            std::cout << "SPRT: inconclusive" << std::endl;
        }
    }
    return 0;
}
