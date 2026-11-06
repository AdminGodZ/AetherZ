#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace aetherz::nnue {

// (768 -> hidden_size)x2 -> 1 with SCReLU, as trained by Bullet's `simple`
// example. The quantisation constants must match the ones used for training.
inline constexpr int input_size = 768;
inline constexpr int hidden_size = 256;
inline constexpr int qa = 255;
inline constexpr int qb = 64;
inline constexpr int eval_scale = 400;

// Bullet pads the quantised network to a multiple of 64 bytes.
inline constexpr std::size_t max_file_padding = 63;
inline constexpr std::size_t network_value_count =
    static_cast<std::size_t>(input_size) * hidden_size + hidden_size + 2 * hidden_size + 1;
inline constexpr std::size_t network_file_size = network_value_count * sizeof(std::int16_t);

struct alignas(64) Accumulator {
    std::array<std::int16_t, hidden_size> values;
};

// Laid out like Bullet's quantised.bin: feature weights (one hidden-size
// column per input feature), feature bias, output weights (side to move's
// half first), output bias.
struct Network {
    std::array<Accumulator, input_size> feature_weights;
    Accumulator feature_bias;
    alignas(64) std::array<std::int16_t, 2 * hidden_size> output_weights;
    std::int16_t output_bias;
    // Set at load time; see evaluate().
    bool small_output_weights;

    // Centipawns from the side to move's point of view.
    [[nodiscard]] int evaluate(const Accumulator& us, const Accumulator& them) const;
};

// Returns nullptr (and sets error) if the file is missing or has the wrong
// size for this architecture.
[[nodiscard]] std::unique_ptr<Network> load_network(std::string_view path, std::string* error = nullptr);
// The same for the contents of a network file; name is used in errors.
[[nodiscard]] std::unique_ptr<Network> parse_network(
    std::span<const unsigned char> bytes, std::string_view name, std::string* error = nullptr);

// The network built into the engine (CMake option AETHERZ_EVALFILE), or an
// empty span. Loading the EvalFile embedded_network_name selects it.
[[nodiscard]] std::span<const unsigned char> embedded_network();
inline constexpr std::string_view embedded_network_name = "<embedded>";

} // namespace aetherz::nnue
