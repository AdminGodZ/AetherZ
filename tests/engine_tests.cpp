#include "engine/benchmark.hpp"
#include "engine/attacks.hpp"
#include "engine/board.hpp"
#include "engine/evaluate.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"
#include "engine/see.hpp"
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

aetherz::Move require_legal_move(const aetherz::Board& board, std::string_view text)
{
    const auto parsed = aetherz::move_from_uci(text);
    require(parsed.has_value(), "test move UCI parses");

    for (const auto& move : board.generate_legal_moves()) {
        if (move.from == parsed->from && move.to == parsed->to && move.promotion == parsed->promotion) {
            return move;
        }
    }

    require(false, "test move is legal");
    return {};
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

void test_null_move_restores_state_exactly()
{
    auto board = aetherz::Board::startpos();
    const auto before_fen = board.to_fen();
    const auto before_hash = board.hash();

    aetherz::Board::UndoState undo;
    require(board.make_null_move(undo), "null move can be made outside check");
    require(board.side_to_move() == aetherz::Color::Black, "null move toggles side to move");
    board.unmake_move(undo);

    require(board.to_fen() == before_fen, "null unmake restores FEN");
    require(board.hash() == before_hash, "null unmake restores hash");
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

void test_pseudo_legal_tactical_generation_skips_quiet_moves()
{
    const auto board = aetherz::Board::from_fen("4k3/1P6/8/8/8/8/4r3/4Q1K1 w - - 0 1");

    require(board.has_value(), "tactical generation FEN parses");

    bool found_capture = false;
    bool found_promotion = false;
    bool found_quiet = false;

    for (const auto& move : board->generate_pseudo_legal_tactical_moves()) {
        const std::string text = aetherz::move_to_uci(move);
        found_capture = found_capture || text == "e1e2";
        found_promotion = found_promotion || text == "b7b8q";
        found_quiet = found_quiet || text == "g1f1";
    }

    require(found_capture, "tactical generation includes captures");
    require(found_promotion, "tactical generation includes quiet promotions");
    require(!found_quiet, "tactical generation excludes quiet non-promotions");
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
    require(result.principal_variation.size() >= 2, "iterative search reports multi-move PV");
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

void test_attack_helpers_find_attackers()
{
    const auto board = aetherz::Board::from_fen("4k3/4q3/8/8/8/8/4r3/4Q1K1 w - - 0 1");

    require(board.has_value(), "attack helper FEN parses");

    const auto e2 = aetherz::square_from_string("e2");
    const auto e1 = aetherz::square_from_string("e1");
    const auto e7 = aetherz::square_from_string("e7");

    require(e2.has_value() && e1.has_value() && e7.has_value(), "attack helper squares parse");
    require((aetherz::attackers_to(*board, *e2, aetherz::Color::White, board->occupancy()) & aetherz::square_bb(*e1)) != 0,
        "white queen attacks e2");
    require((aetherz::attackers_to(*board, *e2, aetherz::Color::Black, board->occupancy()) & aetherz::square_bb(*e7)) != 0,
        "black queen attacks e2");
}

void test_evaluation_rewards_bishop_pair()
{
    const auto bishop_pair = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/2BBK3 w - - 0 1");
    const auto bishop_knight = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/2BNK3 w - - 0 1");

    require(bishop_pair.has_value(), "bishop pair FEN parses");
    require(bishop_knight.has_value(), "bishop knight FEN parses");
    require(aetherz::evaluate_white(*bishop_pair) > aetherz::evaluate_white(*bishop_knight), "evaluation rewards bishop pair");
}

void test_evaluation_rewards_passed_pawn()
{
    const auto passed = aetherz::Board::from_fen("4k3/7p/P7/8/8/8/8/4K3 w - - 0 1");
    const auto blocked = aetherz::Board::from_fen("4k3/p7/P7/8/8/8/8/4K3 w - - 0 1");

    require(passed.has_value(), "passed pawn FEN parses");
    require(blocked.has_value(), "blocked pawn FEN parses");
    require(aetherz::evaluate_white(*passed) > aetherz::evaluate_white(*blocked), "evaluation rewards passed pawn");
}

void test_evaluation_rewards_rook_open_file()
{
    const auto open_file = aetherz::Board::from_fen("4k3/8/8/8/8/8/7P/R3K3 w - - 0 1");
    const auto closed_file = aetherz::Board::from_fen("4k3/8/8/8/8/8/P7/R3K3 w - - 0 1");

    require(open_file.has_value(), "open rook file FEN parses");
    require(closed_file.has_value(), "closed rook file FEN parses");
    require(aetherz::evaluate_white(*open_file) > aetherz::evaluate_white(*closed_file), "evaluation rewards open rook file");
}

void test_static_exchange_evaluation_scores_winning_capture()
{
    const auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/4r3/4Q1K1 w - - 0 1");

    require(board.has_value(), "winning SEE FEN parses");

    const auto move = require_legal_move(*board, "e1e2");

    require(aetherz::static_exchange_evaluation(*board, move) > 400, "SEE scores undefended rook capture as winning");
}

void test_static_exchange_evaluation_scores_losing_capture()
{
    const auto board = aetherz::Board::from_fen("4k3/4q3/8/8/8/8/4r3/4Q1K1 w - - 0 1");

    require(board.has_value(), "losing SEE FEN parses");

    const auto move = require_legal_move(*board, "e1e2");

    require(aetherz::static_exchange_evaluation(*board, move) < 0, "SEE scores protected rook capture as losing");
}

void test_search_finds_mate_in_one()
{
    auto board = aetherz::Board::from_fen("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");

    require(board.has_value(), "mate-in-one FEN parses");

    aetherz::Search search;
    const auto result = search.find_best_move(*board, 2);

    require(result.best_move.has_value(), "mate-in-one search finds a move");
    require(result.score > 29000, "mate-in-one score is near mate score");
}

void test_search_scores_root_checkmate()
{
    auto board = aetherz::Board::from_fen("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");

    require(board.has_value(), "root checkmate FEN parses");

    aetherz::Search search;
    const auto result = search.find_best_move(*board, 2);

    require(!result.best_move.has_value(), "root checkmate has no best move");
    require(result.score < -29000, "root checkmate score is near mate score");
}

void test_search_scores_fifty_move_draw()
{
    auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 99 50");

    require(board.has_value(), "fifty-move draw test FEN parses");

    aetherz::Search search;
    const auto result = search.find_best_move(*board, 1);

    require(result.best_move.has_value(), "fifty-move search still returns a move");
    require(result.score == 0, "quiet move reaching fifty-move limit scores as draw");
}

void test_search_scores_repetition_draw()
{
    auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 0 1");

    require(board.has_value(), "repetition draw test FEN parses");

    aetherz::Search search;
    aetherz::SearchLimits limits;
    limits.max_depth = 2;
    limits.position_history = {board->hash(), board->hash(), board->hash()};

    const auto result = search.find_best_move(*board, limits);

    require(result.best_move.has_value(), "repetition search still returns a move");
    require(result.score == 0, "threefold repetition scores as draw");
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

void test_search_benchmark_command_reports_summary()
{
    std::ostringstream output;
    std::ostringstream error;

    const int status = aetherz::run_search_bench_command({"searchbench", "1"}, output, error);
    const std::string text = output.str();

    require(status == 0, "searchbench command succeeds");
    require(error.str().empty(), "searchbench command has no errors");
    require(text.find("searchbench summary") != std::string::npos, "searchbench reports summary");
}

void test_selfplay_command_reports_summary()
{
    std::ostringstream output;
    std::ostringstream error;

    const int status = aetherz::run_selfplay_command({"selfplay", "1", "1", "2"}, output, error);
    const std::string text = output.str();

    require(status == 0, "selfplay command succeeds");
    require(error.str().empty(), "selfplay command has no errors");
    require(text.find("selfplay summary") != std::string::npos, "selfplay reports summary");
}

void test_tactics_command_passes_regressions()
{
    std::ostringstream output;
    std::ostringstream error;

    const int status = aetherz::run_tactics_command({"tactics", "2"}, output, error);
    const std::string text = output.str();

    require(status == 0, "tactics command succeeds");
    require(error.str().empty(), "tactics command has no errors");
    require(text.find("tactics summary passed") != std::string::npos, "tactics reports passing summary");
}

} // namespace

int main()
{
    test_start_position_fen_round_trips();
    test_make_and_unmake_restores_state_exactly();
    test_null_move_restores_state_exactly();
    test_start_position_perft();
    test_kiwipete_perft();
    test_uci_move_text_parses_promotions();
    test_pseudo_legal_tactical_generation_skips_quiet_moves();
    test_iterative_search_returns_a_move();
    test_time_limited_search_returns_a_move();
    test_repeated_search_uses_transposition_table();
    test_evaluation_is_balanced_at_start();
    test_evaluation_rewards_material();
    test_piece_square_tables_reward_active_knight();
    test_attack_helpers_find_attackers();
    test_evaluation_rewards_bishop_pair();
    test_evaluation_rewards_passed_pawn();
    test_evaluation_rewards_rook_open_file();
    test_static_exchange_evaluation_scores_winning_capture();
    test_static_exchange_evaluation_scores_losing_capture();
    test_search_finds_mate_in_one();
    test_search_scores_root_checkmate();
    test_search_scores_fifty_move_draw();
    test_search_scores_repetition_draw();
    test_uci_go_time_controls();
    test_search_benchmark_command_reports_summary();
    test_selfplay_command_reports_summary();
    test_tactics_command_passes_regressions();

    std::cout << "engine tests passed\n";
    return 0;
}
