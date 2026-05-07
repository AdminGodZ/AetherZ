#include "engine/nnue/network.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>

namespace aetherz::nnue {
namespace {

void assign_error(std::string* error, std::string_view message)
{
    if (error != nullptr) {
        *error = std::string{message};
    }
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

bool parse_int(std::string_view value, int& target)
{
    std::istringstream stream{std::string{value}};
    int parsed = 0;
    if (!(stream >> parsed)) {
        return false;
    }

    target = parsed;
    return true;
}

std::vector<std::int32_t> parse_values(std::istringstream& stream)
{
    std::vector<std::int32_t> values;
    std::int32_t value = 0;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}

bool parse_sized_values(std::istringstream& stream, int size, std::vector<std::int32_t>& target)
{
    target = parse_values(stream);
    return static_cast<int>(target.size()) == size;
}

std::int32_t activate(std::int64_t value)
{
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, 0, 255));
}

std::int64_t scaled(std::int64_t value, int scale)
{
    return value / std::max(1, scale);
}

} // namespace

bool NnueNetwork::load(std::string_view path_view, std::string* error)
{
    const std::string path{path_view};
    std::ifstream input{path};
    if (!input) {
        assign_error(error, "could not open NNUE file");
        clear();
        return false;
    }

    std::string line;
    if (!std::getline(input, line)) {
        assign_error(error, "empty NNUE file");
        clear();
        return false;
    }

    if (trim(line) != native_network_magic) {
        assign_error(error, "unsupported NNUE file format");
        clear();
        return false;
    }

    std::vector<std::string> lines;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty() && line.front() != '#') {
            lines.push_back(line);
        }
    }

    NnueNetworkMetadata parsed_metadata;
    std::vector<std::string> directives;
    for (const std::string& raw : lines) {
        const auto separator = raw.find('=');
        if (separator == std::string::npos) {
            directives.push_back(raw);
            continue;
        }

        const std::string key = trim(raw.substr(0, separator));
        const std::string value = trim(raw.substr(separator + 1));
        if (key == "name" && !value.empty()) {
            parsed_metadata.name = value;
        } else if (key == "feature_set" && !value.empty()) {
            parsed_metadata.feature_set = value;
        } else if (key == "accumulator_size") {
            parse_int(value, parsed_metadata.accumulator_size);
        } else if (key == "hidden_size") {
            parse_int(value, parsed_metadata.hidden_size);
        } else if (key == "input_scale") {
            parse_int(value, parsed_metadata.input_scale);
        } else if (key == "hidden_scale") {
            parse_int(value, parsed_metadata.hidden_scale);
        } else if (key == "output_scale") {
            parse_int(value, parsed_metadata.output_scale);
        }
    }

    if (parsed_metadata.feature_set != "halfka-v1") {
        assign_error(error, "unsupported NNUE feature set");
        clear();
        return false;
    }
    if (parsed_metadata.accumulator_size <= 0 || parsed_metadata.hidden_size <= 0) {
        assign_error(error, "invalid NNUE dimensions");
        clear();
        return false;
    }

    std::vector<std::int32_t> accumulator_bias(static_cast<std::size_t>(parsed_metadata.accumulator_size), 0);
    std::unordered_map<int, std::vector<std::int32_t>> feature_weights;
    std::vector<std::int32_t> hidden1_bias(static_cast<std::size_t>(parsed_metadata.hidden_size), 0);
    std::vector<std::int32_t> hidden1_weights(
        static_cast<std::size_t>(parsed_metadata.accumulator_size * 2 * parsed_metadata.hidden_size),
        0);
    std::vector<std::int32_t> hidden2_bias(static_cast<std::size_t>(parsed_metadata.hidden_size), 0);
    std::vector<std::int32_t> hidden2_weights(
        static_cast<std::size_t>(parsed_metadata.hidden_size * parsed_metadata.hidden_size),
        0);
    std::vector<std::int32_t> output_weights(static_cast<std::size_t>(parsed_metadata.hidden_size), 0);
    std::int32_t output_bias = 0;
    bool saw_output = false;

    for (const std::string& directive : directives) {
        std::istringstream stream{directive};
        std::string command;
        stream >> command;

        if (command == "accumulator_bias") {
            if (!parse_sized_values(stream, parsed_metadata.accumulator_size, accumulator_bias)) {
                assign_error(error, "invalid accumulator_bias row");
                clear();
                return false;
            }
        } else if (command == "feature") {
            int feature = -1;
            stream >> feature;
            std::vector<std::int32_t> values;
            if (feature < 0 || !parse_sized_values(stream, parsed_metadata.accumulator_size, values)) {
                assign_error(error, "invalid feature row");
                clear();
                return false;
            }
            feature_weights[feature] = std::move(values);
        } else if (command == "hidden1_bias") {
            if (!parse_sized_values(stream, parsed_metadata.hidden_size, hidden1_bias)) {
                assign_error(error, "invalid hidden1_bias row");
                clear();
                return false;
            }
        } else if (command == "hidden1_weight") {
            int input_index = -1;
            stream >> input_index;
            std::vector<std::int32_t> values;
            if (input_index < 0
                || input_index >= parsed_metadata.accumulator_size * 2
                || !parse_sized_values(stream, parsed_metadata.hidden_size, values)) {
                assign_error(error, "invalid hidden1_weight row");
                clear();
                return false;
            }
            const auto offset = static_cast<std::size_t>(input_index * parsed_metadata.hidden_size);
            std::copy(values.begin(), values.end(), hidden1_weights.begin() + static_cast<std::ptrdiff_t>(offset));
        } else if (command == "hidden2_bias") {
            if (!parse_sized_values(stream, parsed_metadata.hidden_size, hidden2_bias)) {
                assign_error(error, "invalid hidden2_bias row");
                clear();
                return false;
            }
        } else if (command == "hidden2_weight") {
            int input_index = -1;
            stream >> input_index;
            std::vector<std::int32_t> values;
            if (input_index < 0
                || input_index >= parsed_metadata.hidden_size
                || !parse_sized_values(stream, parsed_metadata.hidden_size, values)) {
                assign_error(error, "invalid hidden2_weight row");
                clear();
                return false;
            }
            const auto offset = static_cast<std::size_t>(input_index * parsed_metadata.hidden_size);
            std::copy(values.begin(), values.end(), hidden2_weights.begin() + static_cast<std::ptrdiff_t>(offset));
        } else if (command == "output_bias") {
            stream >> output_bias;
        } else if (command == "output_weight") {
            if (!parse_sized_values(stream, parsed_metadata.hidden_size, output_weights)) {
                assign_error(error, "invalid output_weight row");
                clear();
                return false;
            }
            saw_output = true;
        }
    }

    loaded_ = true;
    ready_ = saw_output;
    path_ = path;
    metadata_ = std::move(parsed_metadata);
    accumulator_bias_ = std::move(accumulator_bias);
    feature_weights_ = std::move(feature_weights);
    hidden1_bias_ = std::move(hidden1_bias);
    hidden1_weights_ = std::move(hidden1_weights);
    hidden2_bias_ = std::move(hidden2_bias);
    hidden2_weights_ = std::move(hidden2_weights);
    output_weights_ = std::move(output_weights);
    output_bias_ = output_bias;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void NnueNetwork::clear()
{
    loaded_ = false;
    ready_ = false;
    path_.clear();
    metadata_ = NnueNetworkMetadata{};
    accumulator_bias_.clear();
    feature_weights_.clear();
    hidden1_bias_.clear();
    hidden1_weights_.clear();
    hidden2_bias_.clear();
    hidden2_weights_.clear();
    output_weights_.clear();
    output_bias_ = 0;
}

const std::vector<std::int32_t>* NnueNetwork::feature_weights(int feature) const
{
    const auto entry = feature_weights_.find(feature);
    if (entry == feature_weights_.end()) {
        return nullptr;
    }

    return &entry->second;
}

int NnueNetwork::evaluate(Color side_to_move, const AccumulatorState& accumulator) const
{
    if (!ready_ || !accumulator.valid) {
        return 0;
    }

    const int accumulator_size_value = metadata_.accumulator_size;
    const int hidden_size_value = metadata_.hidden_size;
    if (static_cast<int>(accumulator.white.size()) != accumulator_size_value
        || static_cast<int>(accumulator.black.size()) != accumulator_size_value) {
        return 0;
    }

    const auto& stm = side_to_move == Color::White ? accumulator.white : accumulator.black;
    const auto& nstm = side_to_move == Color::White ? accumulator.black : accumulator.white;

    std::vector<std::int32_t> hidden1(static_cast<std::size_t>(hidden_size_value), 0);
    for (int output = 0; output < hidden_size_value; ++output) {
        std::int64_t sum = hidden1_bias_[output];
        for (int input_index = 0; input_index < accumulator_size_value; ++input_index) {
            const auto stm_weight = hidden1_weights_[static_cast<std::size_t>(input_index * hidden_size_value + output)];
            const auto nstm_weight = hidden1_weights_[static_cast<std::size_t>((accumulator_size_value + input_index) * hidden_size_value + output)];
            sum += static_cast<std::int64_t>(activate(stm[input_index])) * stm_weight;
            sum += static_cast<std::int64_t>(activate(nstm[input_index])) * nstm_weight;
        }
        hidden1[output] = activate(scaled(sum, metadata_.input_scale));
    }

    std::vector<std::int32_t> hidden2(static_cast<std::size_t>(hidden_size_value), 0);
    for (int output = 0; output < hidden_size_value; ++output) {
        std::int64_t sum = hidden2_bias_[output];
        for (int input_index = 0; input_index < hidden_size_value; ++input_index) {
            const auto weight = hidden2_weights_[static_cast<std::size_t>(input_index * hidden_size_value + output)];
            sum += static_cast<std::int64_t>(hidden1[input_index]) * weight;
        }
        hidden2[output] = activate(scaled(sum, metadata_.hidden_scale));
    }

    std::int64_t output = output_bias_;
    for (int input_index = 0; input_index < hidden_size_value; ++input_index) {
        output += static_cast<std::int64_t>(hidden2[input_index]) * output_weights_[input_index];
    }

    return static_cast<int>(std::clamp<std::int64_t>(
        scaled(output, metadata_.output_scale),
        -32000,
        32000));
}

} // namespace aetherz::nnue
