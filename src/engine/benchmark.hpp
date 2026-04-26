#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace aetherz {

int run_bench_command(std::ostream& output);
int run_search_bench_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_tactics_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_selfplay_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_epd_test_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_arasan_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);

} // namespace aetherz
