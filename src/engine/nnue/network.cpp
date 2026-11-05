#include "engine/nnue/network.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace aetherz::nnue {
namespace {

void assign_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string architecture_name(int hidden)
{
    return "(768 -> " + std::to_string(hidden) + ")x2 -> 1";
}

std::size_t file_size_for(std::size_t hidden)
{
    return (hidden * (input_size + 3) + 1) * sizeof(std::int16_t);
}

// The hidden size of a (768 -> N)x2 -> 1 network whose padded file has this
// size, or 0 if there is none.
std::size_t guess_hidden_size(std::size_t bytes)
{
    const std::size_t hidden = (bytes / sizeof(std::int16_t) - 1) / (input_size + 3);
    if (hidden == 0) {
        return 0;
    }

    const std::size_t expected = file_size_for(hidden);
    return bytes >= expected && bytes - expected <= max_file_padding ? hidden : 0;
}

// sum of clamp(x, 0, QA)^2 * w over the hidden layer. One product is at most
// 255^2 * 2^15 < 2^31, so it fits int32; the sum is kept unsigned so that it
// wraps like Bullet's i32 reference instead of overflowing.
std::uint32_t screlu_dot(const std::int16_t* accumulator, const std::int16_t* weights)
{
    std::uint32_t sum = 0;
    for (int index = 0; index < hidden_size; ++index) {
        const std::int32_t value = std::clamp<std::int32_t>(accumulator[index], 0, qa);
        sum += static_cast<std::uint32_t>(value * value * weights[index]);
    }
    return sum;
}

// Same result with fewer widening multiplies: value * weight stays within
// int16 when |weight| <= 128, which holds for nets trained with Bullet's
// default weight clipping (1.98 * QB).
std::uint32_t screlu_dot_small_weights(const std::int16_t* accumulator, const std::int16_t* weights)
{
    constexpr std::int16_t low = 0;
    constexpr std::int16_t high = qa;
    std::uint32_t sum = 0;
    for (int index = 0; index < hidden_size; ++index) {
        const std::int16_t value = std::clamp(accumulator[index], low, high);
        const auto product = static_cast<std::int16_t>(value * weights[index]);
        sum += static_cast<std::uint32_t>(product * value);
    }
    return sum;
}

} // namespace

int Network::evaluate(const Accumulator& us, const Accumulator& them) const
{
    const std::int16_t* us_weights = output_weights.data();
    const std::int16_t* them_weights = output_weights.data() + hidden_size;
    const std::uint32_t sum = small_output_weights
        ? screlu_dot_small_weights(us.values.data(), us_weights) + screlu_dot_small_weights(them.values.data(), them_weights)
        : screlu_dot(us.values.data(), us_weights) + screlu_dot(them.values.data(), them_weights);

    // Integer division truncates towards zero, as in Bullet's reference.
    std::int64_t output = static_cast<std::int32_t>(sum) / qa;
    output += output_bias;
    output *= eval_scale;
    output /= qa * qb;
    return static_cast<int>(output);
}

std::unique_ptr<Network> load_network(std::string_view path_view, std::string* error)
{
    const std::string path{path_view};
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        assign_error(error, "could not open " + path);
        return nullptr;
    }

    const auto end = input.tellg();
    if (end < 0) {
        assign_error(error, "could not read " + path);
        return nullptr;
    }

    const auto size = static_cast<std::size_t>(end);
    if (size < network_file_size) {
        assign_error(error, path + " is " + std::to_string(size) + " bytes, but a " + architecture_name(hidden_size)
            + " network needs " + std::to_string(network_file_size) + " bytes");
        return nullptr;
    }
    if (size - network_file_size > max_file_padding) {
        std::string message = path + " is " + std::to_string(size) + " bytes, more than the "
            + std::to_string(network_file_size) + " bytes of a " + architecture_name(hidden_size)
            + " network plus at most 63 bytes of padding";
        if (const std::size_t hidden = guess_hidden_size(size); hidden != 0) {
            message += "; it looks like a " + architecture_name(static_cast<int>(hidden)) + " network";
        }
        assign_error(error, std::move(message));
        return nullptr;
    }

    std::vector<unsigned char> bytes(network_file_size);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        assign_error(error, "could not read " + path);
        return nullptr;
    }

    // Decoded explicitly so that the little-endian file also loads on
    // big-endian hosts.
    std::size_t offset = 0;
    const auto next = [&] {
        const auto value = static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
        offset += 2;
        return static_cast<std::int16_t>(value);
    };

    auto network = std::make_unique<Network>();
    for (Accumulator& column : network->feature_weights) {
        for (std::int16_t& weight : column.values) {
            weight = next();
        }
    }
    for (std::int16_t& bias : network->feature_bias.values) {
        bias = next();
    }
    for (std::int16_t& weight : network->output_weights) {
        weight = next();
    }
    network->output_bias = next();
    network->small_output_weights = std::ranges::all_of(network->output_weights, [](std::int16_t weight) {
        return std::abs(weight) <= 128;
    });
    return network;
}

} // namespace aetherz::nnue
