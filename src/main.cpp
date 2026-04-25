#include "engine/uci.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    if (args.size() > 1 && args[1] == "perft") {
        return aetherz::run_perft_command(
            std::vector<std::string>{args.begin() + 1, args.end()},
            std::cout,
            std::cerr);
    }

    if (args.size() > 1 && args[1] == "divide") {
        return aetherz::run_divide_command(
            std::vector<std::string>{args.begin() + 1, args.end()},
            std::cout,
            std::cerr);
    }

    if (args.size() > 1 && args[1] == "bench") {
        return aetherz::run_bench_command(std::cout);
    }

    aetherz::UciEngine engine;
    engine.loop(std::cin, std::cout);
    return 0;
}
