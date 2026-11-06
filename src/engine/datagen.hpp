#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace aetherz {

struct DatagenConfig {
    std::string output_path;
    std::uint64_t games = 1000;
    int threads = 1;
    // Node budget per move. Searches are limited by nodes, not time, so the
    // data does not depend on machine speed or load.
    std::uint64_t nodes = 5000;
    // Random plies before the engine takes over; one more is added for half
    // of the games so both sides get to move first.
    int random_plies = 8;
    std::uint64_t seed = 0x4145544845525aULL;
    int hash_mb = 16;
    // Network to play with (a file or "<embedded>"); empty for the classical
    // evaluation.
    std::string eval_file;
};

struct DatagenStats {
    bool ok = false;
    std::uint64_t games = 0;
    std::uint64_t positions = 0;
    std::uint64_t white_wins = 0;
    std::uint64_t black_wins = 0;
    std::uint64_t draws = 0;
};

// Plays self-play games and appends training positions to the output file as
// Bullet text: "<fen> | <white-relative centipawns> | <white result 1.0/0.5/0.0>".
// Every game depends only on the seed and its game number, never on how
// games are spread over threads.
// Returns empty stats if the output file or the network cannot be opened;
// error, if given, then says why.
[[nodiscard]] DatagenStats generate_training_data(
    const DatagenConfig& config, std::ostream* progress, std::string* error = nullptr);

int run_datagen_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);

} // namespace aetherz
