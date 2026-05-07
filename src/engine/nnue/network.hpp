#pragma once

#include "engine/nnue/accumulator.hpp"
#include "engine/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aetherz::nnue {

inline constexpr std::string_view native_network_magic = "AETHERZ_NNUE_V1";

struct NnueNetworkMetadata {
    std::string name = "unnamed";
    std::string feature_set = "halfka-v1";
    int accumulator_size = 768;
    int hidden_size = 32;
    int input_scale = 1;
    int hidden_scale = 1;
    int output_scale = 1;
};

class NnueNetwork {
public:
    [[nodiscard]] bool load(std::string_view path, std::string* error = nullptr);
    void clear();

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const NnueNetworkMetadata& metadata() const { return metadata_; }
    [[nodiscard]] int accumulator_size() const { return metadata_.accumulator_size; }
    [[nodiscard]] const std::vector<std::int32_t>& accumulator_bias() const { return accumulator_bias_; }
    [[nodiscard]] const std::vector<std::int32_t>* feature_weights(int feature) const;
    [[nodiscard]] int evaluate(Color side_to_move, const AccumulatorState& accumulator) const;

private:
    bool loaded_ = false;
    bool ready_ = false;
    std::string path_;
    NnueNetworkMetadata metadata_;
    std::vector<std::int32_t> accumulator_bias_;
    std::unordered_map<int, std::vector<std::int32_t>> feature_weights_;
    std::vector<std::int32_t> hidden1_bias_;
    std::vector<std::int32_t> hidden1_weights_;
    std::vector<std::int32_t> hidden2_bias_;
    std::vector<std::int32_t> hidden2_weights_;
    std::vector<std::int32_t> output_weights_;
    std::int32_t output_bias_ = 0;
};

} // namespace aetherz::nnue
