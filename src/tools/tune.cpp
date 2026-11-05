// aetherz-tune: fits the evaluation weights to game results (Texel tuning).
//
// Positions are taken from aetherz_match game logs. Each position is stored
// as its evaluation trace, so the evaluation of any weight vector is a cheap
// dot product, and the weights are fitted by gradient descent (Adam) on the
// squared error between the game result and a sigmoid of the evaluation.

#include "engine/board.hpp"
#include "engine/evaluate.hpp"
#include "engine/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using aetherz::Board;
using aetherz::Color;
using aetherz::EvalTrace;
using aetherz::Move;

struct Coefficient {
    std::int16_t index = 0;
    std::int16_t value = 0;
};

struct Position {
    std::uint32_t first = 0;
    std::uint16_t count = 0;
    std::uint8_t phase = 0;
    float result = 0.5F;
};

struct Dataset {
    std::vector<Position> positions;
    std::vector<Coefficient> coefficients;
};

struct Section {
    std::string_view label;
    int first = 0;
    int count = 1;
};

// Must follow eval_index; used to label the generated weights file.
constexpr std::array<Section, 25> sections{{
    {"material", aetherz::eval_index::material, 6},
    {"pst pawn", aetherz::eval_index::piece_square, 64},
    {"pst knight", aetherz::eval_index::piece_square + 64, 64},
    {"pst bishop", aetherz::eval_index::piece_square + 128, 64},
    {"pst rook", aetherz::eval_index::piece_square + 192, 64},
    {"pst queen", aetherz::eval_index::piece_square + 256, 64},
    {"pst king", aetherz::eval_index::piece_square + 320, 64},
    {"passed pawn by rank", aetherz::eval_index::passed_pawn, 8},
    {"mobility per square", aetherz::eval_index::mobility, 6},
    {"doubled pawn", aetherz::eval_index::doubled_pawn, 1},
    {"isolated pawn", aetherz::eval_index::isolated_pawn, 1},
    {"bishop pair", aetherz::eval_index::bishop_pair, 1},
    {"rook open file", aetherz::eval_index::rook_open_file, 1},
    {"rook semi-open file", aetherz::eval_index::rook_semi_open_file, 1},
    {"rook seventh rank", aetherz::eval_index::rook_seventh_rank, 1},
    {"threat hanging piece", aetherz::eval_index::threat_hanging, 6},
    {"threat by pawn", aetherz::eval_index::threat_by_pawn, 6},
    {"outposts: knight central, knight, bishop central, bishop", aetherz::eval_index::outpost_knight_central, 4},
    {"king closeness", aetherz::eval_index::king_closeness, 6},
    {"king zone presence", aetherz::eval_index::king_zone_presence, 6},
    {"king zone attack", aetherz::eval_index::king_zone_attack, 6},
    {"king open file, adjacent open file", aetherz::eval_index::king_open_file, 2},
    {"pawn shield, pawn shield missing", aetherz::eval_index::pawn_shield, 2},
    {"castled, uncastled with rights, uncastled without rights", aetherz::eval_index::castled, 3},
    {"king walk, king central file", aetherz::eval_index::king_walk, 2},
}};

constexpr bool sections_cover_all_weights()
{
    int next = 0;
    for (const Section& section : sections) {
        if (section.first != next) {
            return false;
        }
        next += section.count;
    }
    return next == aetherz::eval_index::count;
}
static_assert(sections_cover_all_weights(), "tuner sections must list every evaluation weight in order");

bool is_tactical(const Move& move)
{
    return move.flag == aetherz::MoveFlag::Capture
        || move.flag == aetherz::MoveFlag::EnPassant
        || move.flag == aetherz::MoveFlag::Promotion
        || move.flag == aetherz::MoveFlag::PromotionCapture;
}

std::string trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(' ');
    const auto end = text.find_last_not_of(' ');
    return begin == std::string_view::npos ? std::string{} : std::string{text.substr(begin, end - begin + 1)};
}

void add_position(Dataset& data, const Board& board, float result)
{
    EvalTrace trace;
    (void)aetherz::evaluate_white_traced(board, trace);

    Position position;
    position.first = static_cast<std::uint32_t>(data.coefficients.size());
    position.phase = static_cast<std::uint8_t>(trace.phase);
    position.result = result;
    for (int index = 0; index < aetherz::eval_index::count; ++index) {
        if (trace.coefficients[index] != 0) {
            data.coefficients.push_back(Coefficient{
                .index = static_cast<std::int16_t>(index),
                .value = static_cast<std::int16_t>(trace.coefficients[index]),
            });
        }
    }
    position.count = static_cast<std::uint16_t>(data.coefficients.size() - position.first);
    data.positions.push_back(position);
}

// Game log lines: "<result> | <white> - <black> | <reason> | <fen> | <moves>".
// Quiet positions are kept: not in check, and the move played from them is
// not a capture or promotion, so the static evaluation is meaningful.
void load_games(Dataset& data, const std::string& path, int skip_plies)
{
    std::ifstream input{path};
    std::string line;
    int games = 0;
    while (std::getline(input, line)) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto bar = line.find('|', start);
            fields.push_back(trim(std::string_view{line}.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
            if (bar == std::string::npos) {
                break;
            }
            start = bar + 1;
        }
        if (fields.size() < 5) {
            continue;
        }

        float result = 0.5F;
        if (fields[0] == "1-0") {
            result = 1.0F;
        } else if (fields[0] == "0-1") {
            result = 0.0F;
        } else if (fields[0] != "1/2-1/2") {
            continue;
        }

        auto board = Board::from_fen(fields[3]);
        if (!board) {
            continue;
        }

        std::istringstream moves{fields[4]};
        std::string text;
        int ply = 0;
        while (moves >> text) {
            const auto parsed = aetherz::move_from_uci(text);
            std::optional<Move> move;
            if (parsed) {
                for (const Move& legal : board->generate_legal_moves()) {
                    if (legal.from == parsed->from && legal.to == parsed->to && legal.promotion == parsed->promotion) {
                        move = legal;
                        break;
                    }
                }
            }
            if (!move) {
                break;
            }

            if (ply >= skip_plies && !is_tactical(*move) && !board->in_check(board->side_to_move())) {
                add_position(data, *board, result);
            }

            Board::UndoState undo;
            if (!board->make_move(*move, undo)) {
                break;
            }
            ++ply;
        }
        ++games;
    }
    std::cout << "loaded " << games << " games from " << path << ", " << data.positions.size() << " positions in total" << std::endl;
}

using Weights = std::vector<double>; // middlegame and endgame interleaved

double evaluate(const Dataset& data, const Position& position, const Weights& weights)
{
    double middlegame = 0.0;
    double endgame = 0.0;
    for (std::uint32_t offset = 0; offset < position.count; ++offset) {
        const Coefficient& coefficient = data.coefficients[position.first + offset];
        middlegame += coefficient.value * weights[2 * coefficient.index];
        endgame += coefficient.value * weights[2 * coefficient.index + 1];
    }
    const double phase = position.phase / static_cast<double>(aetherz::max_eval_phase);
    return middlegame * phase + endgame * (1.0 - phase);
}

double sigmoid(double k, double eval)
{
    return 1.0 / (1.0 + std::pow(10.0, -k * eval / 400.0));
}

// Splits the positions over threads and sums the per-thread results.
template <typename Work>
void parallel_for(std::size_t count, int threads, Work work)
{
    std::vector<std::thread> workers;
    const std::size_t chunk = (count + threads - 1) / threads;
    for (int thread = 0; thread < threads; ++thread) {
        const std::size_t begin = thread * chunk;
        const std::size_t end = std::min(count, begin + chunk);
        workers.emplace_back([=, &work] { work(thread, begin, end); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

double mean_error(const Dataset& data, const Weights& weights, double k, int threads)
{
    std::vector<double> sums(threads, 0.0);
    parallel_for(data.positions.size(), threads, [&](int thread, std::size_t begin, std::size_t end) {
        double sum = 0.0;
        for (std::size_t index = begin; index < end; ++index) {
            const Position& position = data.positions[index];
            const double error = position.result - sigmoid(k, evaluate(data, position, weights));
            sum += error * error;
        }
        sums[thread] = sum;
    });

    double total = 0.0;
    for (const double sum : sums) {
        total += sum;
    }
    return total / static_cast<double>(data.positions.size());
}

double find_scale(const Dataset& data, const Weights& weights, int threads)
{
    double low = 0.1;
    double high = 3.0;
    for (int iteration = 0; iteration < 40; ++iteration) {
        const double left = low + (high - low) / 3.0;
        const double right = high - (high - low) / 3.0;
        if (mean_error(data, weights, left, threads) < mean_error(data, weights, right, threads)) {
            high = right;
        } else {
            low = left;
        }
    }
    return (low + high) / 2.0;
}

Weights gradient(const Dataset& data, const Weights& weights, double k, int threads)
{
    std::vector<Weights> partials(threads, Weights(weights.size(), 0.0));
    parallel_for(data.positions.size(), threads, [&](int thread, std::size_t begin, std::size_t end) {
        Weights& partial = partials[thread];
        for (std::size_t index = begin; index < end; ++index) {
            const Position& position = data.positions[index];
            const double predicted = sigmoid(k, evaluate(data, position, weights));
            // d(error^2)/d(eval) for the base-10 logistic used above.
            const double slope = -2.0 * (position.result - predicted) * predicted * (1.0 - predicted) * k * std::log(10.0) / 400.0;
            const double phase = position.phase / static_cast<double>(aetherz::max_eval_phase);
            for (std::uint32_t offset = 0; offset < position.count; ++offset) {
                const Coefficient& coefficient = data.coefficients[position.first + offset];
                partial[2 * coefficient.index] += slope * coefficient.value * phase;
                partial[2 * coefficient.index + 1] += slope * coefficient.value * (1.0 - phase);
            }
        }
    });

    Weights total(weights.size(), 0.0);
    for (const Weights& partial : partials) {
        for (std::size_t index = 0; index < total.size(); ++index) {
            total[index] += partial[index];
        }
    }
    for (double& value : total) {
        value /= static_cast<double>(data.positions.size());
    }
    return total;
}

void write_weights(const std::string& path, const Weights& weights)
{
    std::ofstream output{path};
    output << "// Generated by aetherz_tune; edit by re-running the tuner.\n";
    output << "// Evaluation weights as {middlegame, endgame}, in the order of eval_index.\n";
    for (const Section& section : sections) {
        output << "// " << section.label << '\n';
        for (int offset = 0; offset < section.count; ++offset) {
            const int index = section.first + offset;
            output << '{' << std::lround(weights[2 * index]) << ", " << std::lround(weights[2 * index + 1]) << "},";
            output << ((offset % 8 == 7 || offset + 1 == section.count) ? '\n' : ' ');
        }
    }
}

void usage()
{
    std::cerr << "usage: aetherz-tune --out <eval_weights.inc> [--epochs N] [--rate R] [--skip-plies N]\n"
              << "                    [--threads N] <games-file>...\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string output_path;
    int epochs = 2000;
    double rate = 1.0;
    int skip_plies = 16;
    int threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    std::vector<std::string> inputs;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto next = [&]() -> std::string {
                if (index + 1 >= argc) {
                    throw std::invalid_argument{"missing value for " + argument};
                }
                return argv[++index];
            };

            if (argument == "--out") {
                output_path = next();
            } else if (argument == "--epochs") {
                epochs = std::stoi(next());
            } else if (argument == "--rate") {
                rate = std::stod(next());
            } else if (argument == "--skip-plies") {
                skip_plies = std::stoi(next());
            } else if (argument == "--threads") {
                threads = std::max(1, std::stoi(next()));
            } else {
                inputs.push_back(argument);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        usage();
        return 1;
    }

    if (output_path.empty() || inputs.empty()) {
        usage();
        return 1;
    }

    Dataset data;
    for (const std::string& path : inputs) {
        load_games(data, path, skip_plies);
    }
    if (data.positions.empty()) {
        std::cerr << "no positions loaded\n";
        return 1;
    }

    Weights weights(2 * aetherz::eval_index::count);
    const auto& initial = aetherz::eval_weights();
    for (int index = 0; index < aetherz::eval_index::count; ++index) {
        weights[2 * index] = initial[index].middlegame;
        weights[2 * index + 1] = initial[index].endgame;
    }

    // The scale is fitted once to the starting weights and then held fixed,
    // so the weights keep their centipawn meaning.
    const double k = find_scale(data, weights, threads);
    std::cout << std::fixed << std::setprecision(6)
              << "scale K " << k << ", initial error " << mean_error(data, weights, k, threads) << std::endl;

    // Adam.
    constexpr double beta1 = 0.9;
    constexpr double beta2 = 0.999;
    constexpr double epsilon = 1e-8;
    Weights first_moment(weights.size(), 0.0);
    Weights second_moment(weights.size(), 0.0);
    for (int epoch = 1; epoch <= epochs; ++epoch) {
        const Weights grad = gradient(data, weights, k, threads);
        for (std::size_t index = 0; index < weights.size(); ++index) {
            // The king has no material value.
            if (index / 2 == static_cast<std::size_t>(aetherz::eval_index::material + 5)) {
                continue;
            }
            first_moment[index] = beta1 * first_moment[index] + (1.0 - beta1) * grad[index];
            second_moment[index] = beta2 * second_moment[index] + (1.0 - beta2) * grad[index] * grad[index];
            const double corrected_first = first_moment[index] / (1.0 - std::pow(beta1, epoch));
            const double corrected_second = second_moment[index] / (1.0 - std::pow(beta2, epoch));
            weights[index] -= rate * corrected_first / (std::sqrt(corrected_second) + epsilon);
        }

        if (epoch % 100 == 0 || epoch == epochs) {
            std::cout << "epoch " << epoch << " error " << mean_error(data, weights, k, threads) << std::endl;
            write_weights(output_path, weights);
        }
    }

    std::cout << "wrote " << output_path << std::endl;
    return 0;
}
