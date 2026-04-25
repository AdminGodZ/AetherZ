#pragma once

#include "engine/board.hpp"
#include "engine/search.hpp"

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

class UciEngine {
public:
    void loop(std::istream& input, std::ostream& output);

private:
    Board board_ = Board::startpos();
    Search search_;

    void handle_setoption(const std::vector<std::string>& tokens);
    void handle_position(const std::vector<std::string>& tokens);
    void handle_go(const std::vector<std::string>& tokens, std::ostream& output);
    void handle_perft(const std::vector<std::string>& tokens, std::ostream& output);
    void handle_divide(const std::vector<std::string>& tokens, std::ostream& output);
    void handle_moves(std::ostream& output);
    [[nodiscard]] bool apply_uci_move(std::string_view move_text);
};

int run_perft_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_divide_command(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
int run_bench_command(std::ostream& output);

} // namespace aetherz
