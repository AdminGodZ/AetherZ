#include "engine/board.hpp"
#include "engine/evaluate.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"
#include "engine/types.hpp"
#include "engine/uci.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void test_start_position_fen_round_trips()
{
    const auto board = aetherz::Board::from_fen(aetherz::Board::start_fen);

    require(board.has_value(), "start FEN parses");
    require(board->to_fen() == aetherz::Board::start_fen, "start FEN round-trips");
    require(board->side_to_move() == aetherz::Color::White, "start side to move is white");
    require(board->king_square(aetherz::Color::White) == 4, "white king starts on e1");
    require(board->king_square(aetherz::Color::Black) == 60, "black king starts on e8");
}

void test_make_and_unmake_restores_state_exactly()
{
    auto board = aetherz::Board::startpos();
    const auto before_fen = board.to_fen();
    const auto before_hash = board.hash();

    for (const auto& move : board.generate_legal_moves()) {
        aetherz::Board::UndoState undo;
        require(board.make_move(move, undo), "legal move can be made");
        board.unmake_move(undo);
        require(board.to_fen() == before_fen, "unmake restores FEN");
        require(board.hash() == before_hash, "unmake restores hash");
    }
}

void test_start_position_perft()
{
    auto board = aetherz::Board::startpos();

    require(aetherz::perft(board, 1) == 20, "startpos perft depth 1");
    require(aetherz::perft(board, 2) == 400, "startpos perft depth 2");
    require(aetherz::perft(board, 3) == 8902, "startpos perft depth 3");
}

void test_kiwipete_perft()
{
    auto board = aetherz::Board::from_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    require(board.has_value(), "kiwipete FEN parses");
    require(aetherz::perft(*board, 1) == 48, "kiwipete perft depth 1");
    require(aetherz::perft(*board, 2) == 2039, "kiwipete perft depth 2");
}

void test_uci_move_text_parses_promotions()
{
    const auto move = aetherz::move_from_uci("a7a8q");

    require(move.has_value(), "promotion UCI parses");
    require(move->from == 48, "promotion from-square");
    require(move->to == 56, "promotion to-square");
    require(move->promotion == aetherz::PieceType::Queen, "promotion piece");
    require(aetherz::move_to_uci(*move) == "a7a8q", "promotion UCI round-trip");
}

void test_iterative_search_returns_a_move()
{
    auto board = aetherz::Board::startpos();
    aetherz::Search search;
    int completed_depths = 0;

    aetherz::SearchLimits limits;
    limits.max_depth = 3;
    limits.on_depth_finished = [&](const aetherz::SearchResult&) {
        ++completed_depths;
    };

    const auto result = search.find_best_move(board, limits);

    require(result.best_move.has_value(), "iterative search finds a move");
    require(result.depth == 3, "iterative search reaches requested fixed depth");
    require(completed_depths == 3, "iterative search reports each completed depth");
}

void test_time_limited_search_returns_a_move()
{
    auto board = aetherz::Board::startpos();
    aetherz::Search search;

    aetherz::SearchLimits limits;
    limits.max_depth = 64;
    limits.move_time = std::chrono::milliseconds{25};

    const auto result = search.find_best_move(board, limits);

    require(result.best_move.has_value(), "time-limited search finds a move");
    require(result.nodes > 0, "time-limited search visits nodes");
}

void test_repeated_search_uses_transposition_table()
{
    auto board = aetherz::Board::startpos();
    aetherz::Search search;
    search.set_hash_size_mb(1);

    const auto first = search.find_best_move(board, 4);
    const auto second = search.find_best_move(board, 4);

    require(first.best_move.has_value(), "first TT search finds a move");
    require(second.best_move.has_value(), "second TT search finds a move");
    require(second.tt_hits > 0, "repeated search produces TT hits");
}

void test_evaluation_is_balanced_at_start()
{
    const auto board = aetherz::Board::startpos();

    require(aetherz::evaluate_white(board) == 0, "start position evaluates as balanced");
    require(aetherz::evaluate_side_to_move(board) == 0, "side-to-move start eval is balanced");
}

void test_evaluation_rewards_material()
{
    const auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 0 1");

    require(board.has_value(), "material eval test FEN parses");
    require(aetherz::evaluate_white(*board) > 800, "white queen advantage scores strongly positive");
}

void test_piece_square_tables_reward_active_knight()
{
    const auto central = aetherz::Board::from_fen("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    const auto corner = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");

    require(central.has_value(), "central knight FEN parses");
    require(corner.has_value(), "corner knight FEN parses");
    require(aetherz::evaluate_white(*central) > aetherz::evaluate_white(*corner), "PST rewards active knight placement");
}

void test_uci_go_time_controls()
{
    std::istringstream input{
        "uci\n"
        "setoption name Hash value 16\n"
        "isready\n"
        "position startpos moves e2e4 e7e5\n"
        "go movetime 25\n"
        "go wtime 1000 btime 1000 winc 0 binc 0 movestogo 20\n"
        "quit\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("uciok") != std::string::npos, "UCI handshake succeeds");
    require(text.find("option name Hash") != std::string::npos, "UCI advertises Hash option");
    require(text.find("readyok") != std::string::npos, "UCI readiness succeeds");
    require(text.find("info depth") != std::string::npos, "UCI search reports iterative depth info");
    require(text.find("bestmove ") != std::string::npos, "UCI time-control search returns bestmove");
}

} // namespace

int main()
{
    test_start_position_fen_round_trips();
    test_make_and_unmake_restores_state_exactly();
    test_start_position_perft();
    test_kiwipete_perft();
    test_uci_move_text_parses_promotions();
    test_iterative_search_returns_a_move();
    test_time_limited_search_returns_a_move();
    test_repeated_search_uses_transposition_table();
    test_evaluation_is_balanced_at_start();
    test_evaluation_rewards_material();
    test_piece_square_tables_reward_active_knight();
    test_uci_go_time_controls();

    std::cout << "engine tests passed\n";
    return 0;
}
