#include "engine/benchmark.hpp"
#include "engine/attacks.hpp"
#include "engine/board.hpp"
#include "engine/datagen.hpp"
#include "engine/epd.hpp"
#include "engine/evaluate.hpp"
#include "engine/nnue/nnue.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"
#include "engine/see.hpp"
#include "engine/thread.hpp"
#include "engine/types.hpp"
#include "engine/uci.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <string_view>
#include <vector>

namespace {

// A scratch file in the system temporary directory, which also exists on
// Windows (unlike /tmp).
std::string temp_path(std::string_view name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

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

void test_board_piece_cache_tracks_moves()
{
    auto board = aetherz::Board::startpos();
    const auto before_squares = board.squares();
    const auto before_white_occupancy = board.occupancy(aetherz::Color::White);
    const auto before_all_occupancy = board.occupancy();

    const auto move = require_legal_move(board, "e2e4");
    aetherz::Board::UndoState undo;
    require(board.make_move(move, undo), "cached board test move can be made");

    require(board.piece_at(12) == aetherz::Piece::None, "piece cache clears source square");
    require(board.piece_at(28) == aetherz::Piece::WhitePawn, "piece cache fills target square");
    require((board.occupancy(aetherz::Color::White) & aetherz::square_bb(28)) != 0, "white occupancy includes moved pawn");
    require((board.occupancy(aetherz::Color::White) & aetherz::square_bb(12)) == 0, "white occupancy clears source");
    require(board.king_square(aetherz::Color::White) == 4, "king cache remains valid");

    board.unmake_move(undo);
    require(board.squares() == before_squares, "unmake restores square cache");
    require(board.occupancy(aetherz::Color::White) == before_white_occupancy, "unmake restores color occupancy cache");
    require(board.occupancy() == before_all_occupancy, "unmake restores full occupancy cache");
}

void test_incremental_hash_matches_full_hash()
{
    // Walk several games of pseudo-random legal moves, covering castling,
    // en passant and promotions, and compare against a hash rebuilt from FEN.
    const std::array<std::string_view, 3> fens{
        aetherz::Board::start_fen,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    };

    std::uint64_t state = 12345;
    for (const auto fen : fens) {
        auto board = *aetherz::Board::from_fen(fen);
        for (int ply = 0; ply < 200; ++ply) {
            const auto moves = board.generate_legal_moves();
            if (moves.empty()) {
                break;
            }
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            aetherz::Board::UndoState undo;
            require(board.make_move(moves[(state >> 33) % moves.size()], undo), "random legal move is playable");

            const auto rebuilt = aetherz::Board::from_fen(board.to_fen());
            require(rebuilt.has_value() && rebuilt->hash() == board.hash(), "incremental hash matches rebuilt hash");
        }
    }
}

void test_gives_check_matches_making_the_move()
{
    const std::array<std::string_view, 4> fens{
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    };

    // Check every pseudo-legal move along random games from each position.
    std::uint64_t state = 987654321;
    for (const auto fen : fens) {
        auto board = *aetherz::Board::from_fen(fen);
        for (int ply = 0; ply < 300; ++ply) {
            for (const auto& move : board.generate_pseudo_legal_moves()) {
                auto next = board;
                aetherz::Board::UndoState undo;
                if (!next.make_move(move, undo)) {
                    continue;
                }
                const bool checks = next.in_check(next.side_to_move());
                require(board.gives_check(move) == checks, "gives_check agrees with making the move");
            }

            const auto legal = board.generate_legal_moves();
            if (legal.empty()) {
                break;
            }
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            aetherz::Board::UndoState undo;
            require(board.make_move(legal[(state >> 33) % legal.size()], undo), "random legal move is playable");
        }
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

void test_reference_perft_positions()
{
    struct PerftCase {
        std::string_view name;
        std::string_view fen;
        int depth;
        std::uint64_t expected;
    };

    const PerftCase cases[]{
        {"startpos-d4", aetherz::Board::start_fen, 4, 197281},
        {"kiwipete-d3", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
        {"rook-pawn-endgame-d4", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238},
        {"promotion-castling-d4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
        {"promotion-check-d3", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379},
        {"middlegame-castled-d3", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890},
    };

    for (const PerftCase& entry : cases) {
        auto board = aetherz::Board::from_fen(entry.fen);
        require(board.has_value(), "reference perft FEN parses");
        require(aetherz::perft(*board, entry.depth) == entry.expected, entry.name);
    }
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

void test_epd_parser_reads_best_avoid_and_id()
{
    const auto position = aetherz::parse_epd(
        "r1b2rk1/1p1nbppp/pq1p4/3B4/P2NP3/2N1p3/1PP3PP/R2Q1R1K w - - bm Rxf7; am Qh5; id \"sample.1\";");

    require(position.has_value(), "EPD line parses");
    require(position->id == "sample.1", "EPD id parses");
    require(position->best_moves.size() == 1 && position->best_moves.front() == "Rxf7", "EPD bm parses");
    require(position->avoid_moves.size() == 1 && position->avoid_moves.front() == "Qh5", "EPD am parses");

    const auto od_position = aetherz::parse_epd(
        "1q3r1k/6b1/B6p/2P5/1P1p2pQ/P3r3/6P1/1R1R2K1 b - - bm g3; od \"sample.od\";");
    require(od_position.has_value() && od_position->id == "sample.od", "EPD od fallback id parses");
}

void test_epd_move_matching_accepts_san()
{
    const auto board = aetherz::Board::from_fen(
        "r1b2rk1/1p1nbppp/pq1p4/3B4/P2NP3/2N1p3/1PP3PP/R2Q1R1K w - - 0 1");

    require(board.has_value(), "EPD SAN test FEN parses");

    const auto move = require_legal_move(*board, "f1f7");

    require(aetherz::epd_move_matches(*board, "Rxf7", move), "EPD matcher accepts SAN capture");
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

void test_multithreaded_search_finds_mate_and_stops()
{
    auto board = *aetherz::Board::from_fen("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    aetherz::Search search;
    search.set_threads(4);
    require(search.threads() == 4, "search uses the requested thread count");

    aetherz::SearchLimits limits;
    limits.max_depth = aetherz::max_search_depth;
    limits.move_time = std::chrono::milliseconds{100};
    const auto start = std::chrono::steady_clock::now();
    const auto result = search.find_best_move(board, limits);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    require(result.best_move.has_value(), "multithreaded search returns a move");
    require(aetherz::move_to_uci(*result.best_move) == "g6g7", "multithreaded search finds mate in one");
    require(elapsed < std::chrono::seconds{2}, "multithreaded search stops its helpers");

    search.set_threads(1);
    require(search.threads() == 1, "search can return to a single thread");
}

// Uses about 16 KB of stack per call. The call goes through a volatile
// function pointer and reads the caller's buffer at an index only known at
// run time, so the optimizer can neither inline the recursion nor shrink the
// buffers.
long deep_recursion(int depth, const unsigned char* previous);
long (*volatile recurse)(int, const unsigned char*) = deep_recursion;

long deep_recursion(int depth, const unsigned char* previous)
{
    unsigned char buffer[16 * 1024];
    std::fill(std::begin(buffer), std::end(buffer), static_cast<unsigned char>(depth));
    const long below = depth == 0 ? 0 : recurse(depth - 1, buffer);
    const std::size_t index = static_cast<std::size_t>(depth) * 37 % sizeof(buffer);
    return below + (previous == nullptr ? 0 : previous[index] - (depth + 1) % 256);
}

void test_search_object_fits_on_a_small_stack()
{
    // Command-line searches put a Search on the main thread's stack, which
    // is only 1 MB with MSVC on Windows.
    require(sizeof(aetherz::Search) < 64 * 1024, "Search keeps its large tables off the stack");
}

void test_search_thread_has_a_large_stack()
{
    // About 6 MB: more than the default stack of secondary threads on
    // macOS (512 KB) and Windows (1 MB).
    int result = -1;
    aetherz::SearchThread thread{[&result] {
        result = static_cast<int>(deep_recursion(384, nullptr));
    }};
    thread.join();
    require(result == 0, "search thread runs deep recursion");
    require(!thread.joinable(), "joined search thread is no longer joinable");
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

void test_evaluation_trace_rebuilds_evaluation()
{
    const std::array<std::string_view, 3> fens{
        aetherz::Board::start_fen,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    };

    const auto& weights = aetherz::eval_weights();
    for (const auto fen : fens) {
        const auto board = *aetherz::Board::from_fen(fen);
        aetherz::EvalTrace trace;
        const int traced = aetherz::evaluate_white_traced(board, trace);

        long middlegame = 0;
        long endgame = 0;
        for (int index = 0; index < aetherz::eval_index::count; ++index) {
            middlegame += static_cast<long>(trace.coefficients[index]) * weights[index].middlegame;
            endgame += static_cast<long>(trace.coefficients[index]) * weights[index].endgame;
        }
        const long rebuilt = (middlegame * trace.phase + endgame * (aetherz::max_eval_phase - trace.phase)) / aetherz::max_eval_phase;

        require(traced == aetherz::evaluate_white(board), "traced evaluation matches evaluation");
        require(rebuilt == traced, "evaluation trace rebuilds the evaluation from the weights");
    }
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

void test_evaluation_rewards_supported_outpost()
{
    const auto supported = aetherz::Board::from_fen("4k3/7p/p7/3N4/2P1P3/8/8/4K3 w - - 0 1");
    const auto contested = aetherz::Board::from_fen("4k3/7p/4p3/3N4/2P1P3/8/8/4K3 w - - 0 1");

    require(supported.has_value(), "supported outpost FEN parses");
    require(contested.has_value(), "contested outpost FEN parses");
    require(aetherz::evaluate_white(*supported) > aetherz::evaluate_white(*contested), "evaluation rewards supported outpost");
}

void test_evaluation_prefers_castling_to_king_walk()
{
    const auto board = aetherz::Board::from_fen(
        "r3k2r/ppp2ppp/2n1pn2/3p4/3P4/2N1PN2/PPP2PPP/R3K2R w KQkq - 0 1");

    require(board.has_value(), "castling eval FEN parses");

    auto castled = *board;
    auto king_walk = *board;
    const auto castle_move = require_legal_move(castled, "e1g1");
    const auto walk_move = require_legal_move(king_walk, "e1d2");

    aetherz::Board::UndoState castle_undo;
    aetherz::Board::UndoState walk_undo;
    require(castled.make_move(castle_move, castle_undo), "castle move can be made");
    require(king_walk.make_move(walk_move, walk_undo), "king walk move can be made");

    require(aetherz::evaluate_white(castled) > aetherz::evaluate_white(king_walk), "evaluation prefers castling to king walk");
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

void test_search_prefers_castling_over_king_walk()
{
    auto board = aetherz::Board::from_fen(
        "r3k2r/ppp2ppp/2n1pn2/3p4/3P4/2N1PN2/PPP2PPP/R3K2R w KQkq - 0 1");

    require(board.has_value(), "castling search FEN parses");

    // Developing or waiting moves are fine; what must not happen is a king
    // step that throws away both castling rights.
    aetherz::Search search;
    const auto result = search.find_best_move(*board, 8);

    require(result.best_move.has_value(), "castling search finds a move");
    const auto moved = board->piece_at(result.best_move->from);
    const bool king_walk = aetherz::piece_type(moved) == aetherz::PieceType::King
        && result.best_move->flag != aetherz::MoveFlag::KingCastle
        && result.best_move->flag != aetherz::MoveFlag::QueenCastle;
    require(!king_walk, "search does not walk the king instead of castling");
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
    // The halfmove clock covers the repeated history below.
    auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 8 5");

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
    require(text.find("option name UseNNUE") != std::string::npos, "UCI advertises UseNNUE option");
    require(text.find("option name EvalFile") != std::string::npos, "UCI advertises EvalFile option");
    require(text.find("readyok") != std::string::npos, "UCI readiness succeeds");
    require(text.find("info depth") != std::string::npos, "UCI search reports iterative depth info");
    require(text.find("bestmove ") != std::string::npos, "UCI time-control search returns bestmove");
}

std::uint64_t next_random(std::uint64_t& state)
{
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
}

// Writes a network in Bullet's quantised layout: little-endian int16 values
// followed by `padding` bytes, which Bullet fills with "bullet".
void write_network(const std::string& path, const std::vector<std::int16_t>& values, std::size_t padding = 0)
{
    require(values.size() == aetherz::nnue::network_value_count, "test network has the expected number of values");
    std::ofstream output{path, std::ios::binary};
    for (const std::int16_t value : values) {
        const auto bits = static_cast<std::uint16_t>(value);
        output.put(static_cast<char>(bits & 0xFF));
        output.put(static_cast<char>(bits >> 8));
    }
    constexpr std::string_view pattern = "bullet";
    for (std::size_t index = 0; index < padding; ++index) {
        output.put(pattern[index % pattern.size()]);
    }
}

// A deterministic pseudo-random network with weights in [-64, 63], about the
// size of a trained network's.
std::string write_random_network(std::string_view name, std::size_t padding = 0)
{
    std::vector<std::int16_t> values(aetherz::nnue::network_value_count);
    std::uint64_t state = 0x5EED;
    for (std::int16_t& value : values) {
        value = static_cast<std::int16_t>(static_cast<int>(next_random(state) % 128) - 64);
    }

    const std::string path = temp_path(name);
    write_network(path, values, padding);
    return path;
}

constexpr std::size_t feature_weight_offset(int feature, int neuron)
{
    return static_cast<std::size_t>(feature) * aetherz::nnue::hidden_size + static_cast<std::size_t>(neuron);
}

constexpr std::size_t feature_bias_offset(int neuron)
{
    return feature_weight_offset(aetherz::nnue::input_size, neuron);
}

constexpr std::size_t output_weight_offset(int index)
{
    return feature_bias_offset(aetherz::nnue::hidden_size) + static_cast<std::size_t>(index);
}

constexpr std::size_t output_bias_offset()
{
    return output_weight_offset(2 * aetherz::nnue::hidden_size);
}

// The same position with colours swapped and the board flipped vertically.
std::string mirror_fen(std::string_view fen)
{
    std::istringstream stream{std::string{fen}};
    std::string placement;
    std::string side;
    std::string castling;
    std::string en_passant;
    std::string halfmove;
    std::string fullmove;
    stream >> placement >> side >> castling >> en_passant >> halfmove >> fullmove;

    const auto swap_case = [](char value) {
        const auto letter = static_cast<unsigned char>(value);
        return static_cast<char>(std::isupper(letter) ? std::tolower(letter) : std::toupper(letter));
    };

    std::vector<std::string> ranks;
    std::istringstream rank_stream{placement};
    for (std::string rank; std::getline(rank_stream, rank, '/');) {
        std::ranges::transform(rank, rank.begin(), swap_case);
        ranks.push_back(rank);
    }
    std::ranges::reverse(ranks);
    std::string mirrored;
    for (const std::string& rank : ranks) {
        mirrored += (mirrored.empty() ? "" : "/") + rank;
    }

    std::string mirrored_castling;
    for (const char right : std::string_view{"KQkq"}) {
        if (castling.find(swap_case(right)) != std::string::npos) {
            mirrored_castling += right;
        }
    }
    if (mirrored_castling.empty()) {
        mirrored_castling = "-";
    }
    if (en_passant != "-") {
        en_passant[1] = en_passant[1] == '3' ? '6' : '3';
    }

    return mirrored + (side == "w" ? " b " : " w ") + mirrored_castling + " " + en_passant + " " + halfmove + " " + fullmove;
}

bool is_legal_move(const aetherz::Board& board, const aetherz::Move& move)
{
    return std::ranges::any_of(board.generate_legal_moves(), [&](const aetherz::Move& legal) {
        return legal.from == move.from && legal.to == move.to && legal.promotion == move.promotion;
    });
}

constexpr std::string_view kiwipete_fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

void test_nnue_loads_bullet_sized_files()
{
    const std::size_t size = aetherz::nnue::network_file_size;
    require(size == 2 * (768 * 256 + 256 + 2 * 256 + 1), "network file size matches the architecture");

    aetherz::nnue::NnueEvaluator evaluator;
    std::string error;
    const std::string exact = write_random_network("aetherz_random_exact.nnue");
    require(evaluator.load(exact, &error), "network of the exact size loads");
    require(evaluator.loaded() && evaluator.eval_file() == exact, "loaded network reports its file");

    const std::string padded = write_random_network("aetherz_random_padded.nnue", (64 - size % 64) % 64);
    require(std::filesystem::file_size(padded) % 64 == 0, "padded test network is a multiple of 64 bytes");
    require(evaluator.load(padded, &error), "network padded to 64 bytes like Bullet's loads");

    const auto board = *aetherz::Board::from_fen(kiwipete_fen);
    aetherz::nnue::NnueEvaluator exact_evaluator;
    require(exact_evaluator.load(exact), "exact network reloads");
    require(
        evaluator.evaluate_from_scratch(board) == exact_evaluator.evaluate_from_scratch(board),
        "padding does not change the network");

    const std::string too_small = temp_path("aetherz_too_small.nnue");
    {
        std::ofstream output{too_small, std::ios::binary};
        output << std::string(size - 2, '\0');
    }
    error.clear();
    require(!evaluator.load(too_small, &error), "too small network file is rejected");
    require(error.find("needs " + std::to_string(size) + " bytes") != std::string::npos, "too small network reports the needed size");
    require(!evaluator.loaded() && evaluator.eval_file().empty(), "failed load leaves no network");
    require(evaluator.evaluate(board) == aetherz::evaluate_side_to_move(board), "without a network the evaluator is classical");

    const std::string too_large = write_random_network("aetherz_too_large.nnue", 64);
    error.clear();
    require(!evaluator.load(too_large, &error), "network file with more than 63 bytes of padding is rejected");
    require(error.find("padding") != std::string::npos, "oversized network reports the padding limit");

    error.clear();
    require(!evaluator.load(temp_path("aetherz_missing_network.nnue"), &error), "missing network file is rejected");
    require(error.find("could not open") != std::string::npos, "missing network reports open failure");
}

void test_nnue_parses_networks_from_memory()
{
    const std::string path = write_random_network("aetherz_random_memory.nnue", 62);
    std::ifstream input{path, std::ios::binary};
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    require(bytes.size() == aetherz::nnue::network_file_size + 62, "test network has the expected size");

    const auto from_file = aetherz::nnue::load_network(path);
    const auto from_memory = aetherz::nnue::parse_network(bytes, "memory");
    require(from_file && from_memory, "network loads from a file and from memory");
    require(
        from_file->feature_weights[767].values == from_memory->feature_weights[767].values
            && from_file->output_weights == from_memory->output_weights
            && from_file->output_bias == from_memory->output_bias,
        "file and memory networks are the same");

    std::string error;
    const std::span<const unsigned char> truncated{bytes.data(), aetherz::nnue::network_file_size - 2};
    require(!aetherz::nnue::parse_network(truncated, "memory", &error), "truncated network is rejected");
    require(error.find("memory is ") == 0, "memory network errors use the given name");

    // "<embedded>" selects the network built in with AETHERZ_EVALFILE.
    aetherz::nnue::NnueEvaluator evaluator;
    const bool built_in = !aetherz::nnue::embedded_network().empty();
    require(
        evaluator.load(aetherz::nnue::embedded_network_name) == built_in,
        "the built-in network loads exactly when there is one");
    require(
        evaluator.eval_file() == (built_in ? std::string{aetherz::nnue::embedded_network_name} : std::string{}),
        "the built-in network reports its name");
}

void test_nnue_matches_hand_computed_evaluation()
{
    using aetherz::nnue::hidden_size;
    std::vector<std::int16_t> values(aetherz::nnue::network_value_count, 0);

    // White's queen on d1: an own queen on square 3 for White, an opponent's
    // queen on square 3 ^ 56 = 59 for Black.
    const int white_view = aetherz::nnue::feature_index(aetherz::Color::White, aetherz::Piece::WhiteQueen, 3);
    const int black_view = aetherz::nnue::feature_index(aetherz::Color::Black, aetherz::Piece::WhiteQueen, 3);
    require(white_view == 4 * 64 + 3, "own queen feature index");
    require(black_view == 384 + 4 * 64 + 59, "opponent queen feature index");

    values[feature_weight_offset(white_view, 0)] = 200;
    values[feature_weight_offset(white_view, 2)] = 300; // clipped to QA
    values[feature_weight_offset(white_view, 3)] = -50; // clipped to 0
    values[feature_weight_offset(black_view, 1)] = 100;
    values[feature_bias_offset(4)] = 30;
    values[output_weight_offset(0)] = 10;
    values[output_weight_offset(2)] = 1;
    values[output_weight_offset(3)] = 100;
    values[output_weight_offset(4)] = 2;
    values[output_weight_offset(hidden_size + 1)] = -20;
    values[output_weight_offset(hidden_size + 4)] = 3;
    values[output_bias_offset()] = -3000;

    const std::string path = temp_path("aetherz_hand_built.nnue");
    write_network(path, values);
    aetherz::nnue::NnueEvaluator evaluator;
    require(evaluator.load(path), "hand-built network loads");

    // White to move: us = White gives 200^2 * 10 + 255^2 * 1 + 30^2 * 2,
    // them = Black gives 100^2 * -20 + 30^2 * 3, so the sum is 269525.
    // 269525 / 255 = 1056; 1056 - 3000 = -1944; -1944 * 400 / (255 * 64) = -47.6 -> -47.
    const auto white = *aetherz::Board::from_fen("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    evaluator.refresh(white);
    require(evaluator.evaluate(white) == -47, "hand-built network evaluates White to move exactly");

    // Black to move: us = Black gives 30^2 * 2 (neuron 1 has no weight on
    // this side), them = White gives 30^2 * 3, so the sum is 4500.
    // 4500 / 255 = 17; 17 - 3000 = -2983; -2983 * 400 / 16320 = -73.1 -> -73.
    const auto black = *aetherz::Board::from_fen("4k3/8/8/8/8/8/8/3QK3 b - - 0 1");
    evaluator.refresh(black);
    require(evaluator.evaluate(black) == -73, "hand-built network evaluates Black to move exactly");

    // An output weight above 128 takes the full int32 path: neuron 7 adds
    // 10^2 * 1000 = 100000 for the side to move.
    // White: 369525 / 255 = 1449; -1551 * 400 / 16320 = -38.0 -> -38.
    // Black: 104500 / 255 = 409; -2591 * 400 / 16320 = -63.5 -> -63.
    values[feature_bias_offset(7)] = 10;
    values[output_weight_offset(7)] = 1000;
    write_network(path, values);
    require(evaluator.load(path), "hand-built network with a large output weight loads");
    evaluator.refresh(white);
    require(evaluator.evaluate(white) == -38, "large output weight evaluates White to move exactly");
    evaluator.refresh(black);
    require(evaluator.evaluate(black) == -63, "large output weight evaluates Black to move exactly");
}

void test_nnue_incremental_updates_match_refresh()
{
    const std::string path = write_random_network("aetherz_random_incremental.nnue");
    aetherz::nnue::NnueEvaluator evaluator;
    require(evaluator.load(path), "random network loads");

    const std::array<std::string_view, 5> fens = {
        aetherz::Board::start_fen,
        kiwipete_fen,
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        "8/1P2k3/8/2pP4/8/8/4Kp2/8 w - c6 0 1",
    };

    std::uint64_t state = 2024;
    int castles = 0;
    int en_passants = 0;
    int promotions = 0;
    int captures = 0;
    int null_moves = 0;
    for (const std::string_view fen : fens) {
        for (int game = 0; game < 4; ++game) {
            auto board = *aetherz::Board::from_fen(fen);
            evaluator.refresh(board);
            std::vector<aetherz::Board::UndoState> undos;
            std::vector<int> evals{evaluator.evaluate(board)};
            require(evals.back() == evaluator.evaluate_from_scratch(board), "refresh matches a fresh evaluation");

            for (int ply = 0; ply < 100; ++ply) {
                aetherz::Board::UndoState undo;
                if (next_random(state) % 8 == 0 && board.make_null_move(undo)) {
                    ++null_moves;
                } else {
                    const auto moves = board.generate_legal_moves();
                    if (moves.empty()) {
                        break;
                    }

                    // Prefer the rare move kinds so that the games exercise them.
                    aetherz::MoveList special;
                    for (const auto& move : moves) {
                        if (move.flag == aetherz::MoveFlag::KingCastle || move.flag == aetherz::MoveFlag::QueenCastle
                            || move.flag == aetherz::MoveFlag::EnPassant || move.promotion != aetherz::PieceType::None) {
                            special.push_back(move);
                        }
                    }
                    const auto& choices = !special.empty() && next_random(state) % 2 == 0 ? special : moves;
                    const auto move = choices[next_random(state) % choices.size()];
                    require(board.make_move(move, undo), "random legal move can be made");

                    castles += move.flag == aetherz::MoveFlag::KingCastle || move.flag == aetherz::MoveFlag::QueenCastle;
                    en_passants += move.flag == aetherz::MoveFlag::EnPassant;
                    promotions += move.promotion != aetherz::PieceType::None;
                    captures += undo.captured != aetherz::Piece::None;
                }

                evaluator.push(aetherz::nnue::dirty_move_from(board, undo));
                undos.push_back(undo);
                evals.push_back(evaluator.evaluate(board));
                require(evals.back() == evaluator.evaluate_from_scratch(board), "incremental update matches a refresh");
            }

            while (!undos.empty()) {
                evaluator.pop();
                board.unmake_move(undos.back());
                undos.pop_back();
                evals.pop_back();
                require(evaluator.evaluate(board) == evals.back(), "pop restores the previous evaluation");
            }
        }
    }

    require(castles > 0, "random games include castling");
    require(en_passants > 0, "random games include en passant");
    require(promotions > 0, "random games include promotions");
    require(captures > 0, "random games include captures");
    require(null_moves > 0, "random games include null moves");

    // A line deeper than the accumulator stack falls back to full
    // evaluations and unwinds cleanly.
    auto board = *aetherz::Board::from_fen(kiwipete_fen);
    evaluator.refresh(board);
    const int root = evaluator.evaluate(board);
    std::vector<aetherz::Board::UndoState> undos;
    for (int ply = 0; ply < aetherz::nnue::accumulator_stack_size + 10; ++ply) {
        aetherz::Board::UndoState undo;
        require(board.make_null_move(undo), "null move can be made");
        evaluator.push(aetherz::nnue::DirtyMove{});
        undos.push_back(undo);
        require(evaluator.evaluate(board) == evaluator.evaluate_from_scratch(board), "deep line evaluates correctly");
    }
    while (!undos.empty()) {
        evaluator.pop();
        board.unmake_move(undos.back());
        undos.pop_back();
    }
    require(evaluator.evaluate(board) == root, "deep line unwinds to the root evaluation");
}

void test_nnue_evaluation_is_colour_symmetric()
{
    const std::string path = write_random_network("aetherz_random_symmetry.nnue");
    aetherz::nnue::NnueEvaluator evaluator;
    require(evaluator.load(path), "random network loads");

    const std::array<std::string_view, 5> fens = {
        aetherz::Board::start_fen,
        kiwipete_fen,
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        "8/1P2k3/8/2pP4/8/8/4Kp2/8 b - - 0 1",
    };

    std::uint64_t state = 77;
    int checked = 0;
    for (const std::string_view fen : fens) {
        auto board = *aetherz::Board::from_fen(fen);
        for (int ply = 0; ply < 30; ++ply) {
            const std::string mirrored_fen = mirror_fen(board.to_fen());
            const auto mirrored = aetherz::Board::from_fen(mirrored_fen);
            require(mirrored.has_value(), "mirrored FEN parses");
            require(mirror_fen(mirrored_fen) == board.to_fen(), "mirroring twice gives the original FEN");
            require(
                evaluator.evaluate_from_scratch(board) == evaluator.evaluate_from_scratch(*mirrored),
                "network evaluates a position and its colour-flipped mirror identically");
            ++checked;

            const auto moves = board.generate_legal_moves();
            if (moves.empty()) {
                break;
            }
            aetherz::Board::UndoState undo;
            require(board.make_move(moves[next_random(state) % moves.size()], undo), "random legal move can be made");
        }
    }
    require(checked > 50, "symmetry is checked along random games");
}

void test_search_with_nnue_returns_legal_moves()
{
    const std::string path = write_random_network("aetherz_random_search.nnue");
    const auto start = *aetherz::Board::from_fen(kiwipete_fen);

    aetherz::Search classical;
    auto classical_board = start;
    const auto expected = classical.find_best_move(classical_board, 6);

    aetherz::Search search;
    std::string error;
    require(search.load_nnue(path, &error), "search loads the network");
    require(search.nnue_loaded() && search.nnue_eval_file() == path, "search reports the loaded network");
    auto board = start;
    const auto result = search.find_best_move(board, 6);
    require(board.to_fen() == start.to_fen(), "NNUE search restores the board");
    require(result.best_move && is_legal_move(start, *result.best_move), "NNUE search returns a legal move");
    require(result.depth == 6, "NNUE search reaches the requested depth");
    require(result.nodes != expected.nodes, "NNUE search uses the network");

    aetherz::Search fallback;
    require(fallback.load_nnue(path), "fallback search loads the network");
    fallback.set_use_nnue(false);
    auto fallback_board = start;
    const auto classical_result = fallback.find_best_move(fallback_board, 6);
    require(classical_result.nodes == expected.nodes, "UseNNUE false searches like the classical evaluation");
    require(classical_result.score == expected.score, "UseNNUE false scores like the classical evaluation");

    // Helpers created before and after loading share the network.
    aetherz::Search threaded_before;
    threaded_before.set_threads(3);
    require(threaded_before.load_nnue(path), "threaded search loads the network");
    auto threaded_board = start;
    const auto threaded_result = threaded_before.find_best_move(threaded_board, 6);
    require(
        threaded_result.best_move && is_legal_move(start, *threaded_result.best_move),
        "threaded NNUE search returns a legal move");

    aetherz::Search threaded_after;
    require(threaded_after.load_nnue(path), "search loads the network before adding threads");
    threaded_after.set_threads(2);
    auto mate_board = *aetherz::Board::from_fen("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    const auto mate_result = threaded_after.find_best_move(mate_board, 6);
    require(
        mate_result.best_move && aetherz::move_to_uci(*mate_result.best_move) == "g6g7",
        "threaded NNUE search finds mate");

    threaded_after.clear_nnue();
    require(!threaded_after.nnue_loaded(), "clearing the network unloads it");
    auto cleared_board = start;
    require(threaded_after.find_best_move(cleared_board, 4).best_move.has_value(), "search runs after clearing the network");
}

std::size_t count_occurrences(const std::string& text, std::string_view needle)
{
    std::size_t count = 0;
    for (auto position = text.find(needle); position != std::string::npos; position = text.find(needle, position + 1)) {
        ++count;
    }
    return count;
}

void test_uci_infinite_search_waits_for_stop()
{
    std::istringstream input{
        "position startpos\n"
        "go infinite\n"
        "isready\n"
        "stop\n"
        "quit\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("readyok") != std::string::npos, "UCI answers isready during a search");
    require(count_occurrences(text, "bestmove ") == 1, "infinite search reports exactly one bestmove after stop");
}

void test_search_stopped_at_once_returns_a_legal_move()
{
    const auto board = *aetherz::Board::from_fen(kiwipete_fen);
    aetherz::Search search;
    aetherz::SearchLimits limits;
    limits.max_depth = 20;
    limits.position_history = {board.hash()};
    limits.on_search_started = [&search] { search.request_stop(); };

    aetherz::Board search_board = board;
    const aetherz::SearchResult result = search.find_best_move(search_board, limits);
    require(result.best_move && is_legal_move(board, *result.best_move), "search stopped at once still returns a legal move");
    require(result.depth == 1, "search stopped at once only completes depth 1");

    std::istringstream input{
        "position startpos\n"
        "go infinite\n"
        "stop\n"
        "quit\n"};
    std::ostringstream output;
    aetherz::UciEngine engine;
    engine.loop(input, output);
    require(output.str().find("bestmove 0000") == std::string::npos, "UCI never reports a null bestmove after stop");
}

void test_uci_ignores_a_byte_order_mark()
{
    std::istringstream input{"\xEF\xBB\xBFuci\nisready\nquit\n"};
    std::ostringstream output;
    aetherz::UciEngine engine;
    engine.loop(input, output);
    require(output.str().find("uciok") != std::string::npos, "UCI answers a first command after a byte order mark");
}

void test_uci_depth_search_finishes_at_end_of_input()
{
    std::istringstream input{
        "position startpos\n"
        "go depth 3\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("info depth 3 ") != std::string::npos, "depth search completes when input ends");
    require(count_occurrences(text, "bestmove ") == 1, "depth search reports one bestmove");
}

void test_uci_reports_mate_scores()
{
    std::istringstream input{
        "position fen 7k/8/5KQ1/8/8/8/8/8 w - - 0 1\n"
        "go depth 3\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("score mate 1 ") != std::string::npos, "UCI reports mate in one as a mate score");
    require(text.find("bestmove g6g7") != std::string::npos, "UCI plays the mating move");
}

void test_uci_ponder_waits_for_ponderhit()
{
    std::istringstream input{
        "position startpos moves e2e4\n"
        "go ponder wtime 2000 btime 2000\n"
        "ponderhit\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(count_occurrences(text, "bestmove ") == 1, "ponder search reports bestmove after ponderhit");
}

void test_uci_movetime_uses_the_full_time()
{
    std::istringstream input{
        "position startpos\n"
        "go movetime 400\n"};
    std::ostringstream output;

    const auto start = std::chrono::steady_clock::now();
    aetherz::UciEngine engine;
    engine.loop(input, output);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    require(output.str().find("bestmove ") != std::string::npos, "movetime search reports bestmove");
    // 400 ms minus the 30 ms default move overhead, with some slack.
    require(elapsed >= std::chrono::milliseconds{330}, "movetime search does not stop early");
    require(elapsed < std::chrono::milliseconds{1500}, "movetime search stops at its limit");
}

void test_time_plan_stays_within_clock()
{
    for (const int remaining : {50, 1000, 10000, 60000, 600000}) {
        for (const int increment : {0, 100, 2000}) {
            const auto plan = aetherz::plan_time(remaining, increment, std::nullopt, 30);
            require(plan.soft.has_value() && plan.hard.has_value(), "time plan sets both limits");
            require(plan.soft->count() >= 1 && plan.soft->count() <= plan.hard->count(), "soft limit is at most hard limit");
            require(plan.hard->count() < std::max(remaining, 2), "hard limit stays below remaining time");
        }
    }

    const auto last_move = aetherz::plan_time(10000, 0, 1, 30);
    require(last_move.soft->count() > 3000, "single move to go uses a large share of the clock");
}

void test_uci_accepts_nnue_options()
{
    const std::string path = write_random_network("aetherz_uci_random.nnue");
    const std::string bad_path = temp_path("aetherz_uci_bad.nnue");
    {
        std::ofstream output{bad_path, std::ios::binary};
        output << "not a network";
    }

    std::istringstream input{
        std::string{"uci\nsetoption name UseNNUE value true\n"}
        + "setoption name EvalFile value " + bad_path + "\n"
        + "setoption name EvalFile value " + path + "\n"
        + "isready\nposition startpos\ngo depth 4\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("option name UseNNUE") != std::string::npos, "UCI exposes UseNNUE");
    require(text.find("option name EvalFile") != std::string::npos, "UCI exposes EvalFile");
    require(text.find("info string failed to load NNUE: ") != std::string::npos, "UCI reports a bad network file");
    require(text.find("info string loaded NNUE " + path) != std::string::npos, "UCI loads the network");
    require(text.find("readyok") != std::string::npos, "UCI remains ready after NNUE load");
    require(text.find("info depth 4 ") != std::string::npos, "UCI searches with the network");
    require(count_occurrences(text, "bestmove ") == 1, "UCI NNUE search reports one bestmove");
}

std::vector<std::string> read_lines(const std::string& path)
{
    std::ifstream input{path};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

aetherz::DatagenStats run_datagen(
    const std::string& path, std::uint64_t games, int threads, const std::string& eval_file = {})
{
    std::filesystem::remove(path);
    aetherz::DatagenConfig config;
    config.output_path = path;
    config.games = games;
    config.threads = threads;
    config.nodes = 400;
    config.hash_mb = 1;
    config.eval_file = eval_file;
    return aetherz::generate_training_data(config, nullptr);
}

void test_datagen_writes_valid_bullet_text()
{
    const std::string path = temp_path("aetherz_datagen_sample.txt");
    const auto stats = run_datagen(path, 3, 1);
    const auto lines = read_lines(path);

    require(stats.ok && stats.games == 3, "datagen plays the requested games");
    require(stats.white_wins + stats.black_wins + stats.draws == 3, "datagen counts every result");
    require(stats.positions > 0 && lines.size() == stats.positions, "datagen writes one line per position");

    for (const std::string& line : lines) {
        const auto first = line.find(" | ");
        const auto second = line.find(" | ", first + 3);
        require(first != std::string::npos && second != std::string::npos, "datagen line has three fields");

        const auto board = aetherz::Board::from_fen(line.substr(0, first));
        require(board.has_value(), "datagen FEN parses");
        require(!board->in_check(board->side_to_move()), "datagen skips positions in check");

        const int score = std::stoi(line.substr(first + 3, second - first - 3));
        require(std::abs(score) < aetherz::mate_bound, "datagen skips mate scores");

        const std::string result = line.substr(second + 3);
        require(result == "1.0" || result == "0.5" || result == "0.0", "datagen writes a white-relative result");
    }
}

void test_datagen_is_independent_of_thread_count()
{
    const std::string single = temp_path("aetherz_datagen_single.txt");
    const std::string threaded = temp_path("aetherz_datagen_threaded.txt");
    (void)run_datagen(single, 4, 1);
    (void)run_datagen(threaded, 4, 3);

    auto single_lines = read_lines(single);
    auto threaded_lines = read_lines(threaded);
    std::sort(single_lines.begin(), single_lines.end());
    std::sort(threaded_lines.begin(), threaded_lines.end());
    require(!single_lines.empty() && single_lines == threaded_lines, "datagen output does not depend on thread count");
}

void test_datagen_plays_with_a_network()
{
    const std::string network = write_random_network("aetherz_datagen_network.nnue");
    const std::string classical = temp_path("aetherz_datagen_classical.txt");
    const std::string single = temp_path("aetherz_datagen_nnue_single.txt");
    const std::string threaded = temp_path("aetherz_datagen_nnue_threaded.txt");
    (void)run_datagen(classical, 2, 1);
    const auto stats = run_datagen(single, 2, 1, network);
    (void)run_datagen(threaded, 2, 2, network);

    auto single_lines = read_lines(single);
    auto threaded_lines = read_lines(threaded);
    require(stats.ok && stats.games == 2 && !single_lines.empty(), "datagen plays games with a network");
    require(single_lines != read_lines(classical), "datagen with a network differs from the classical evaluation");
    std::sort(single_lines.begin(), single_lines.end());
    std::sort(threaded_lines.begin(), threaded_lines.end());
    require(single_lines == threaded_lines, "datagen with a network does not depend on thread count");

    std::string error;
    aetherz::DatagenConfig config;
    config.output_path = temp_path("aetherz_datagen_unused.txt");
    config.eval_file = temp_path("aetherz_missing_network.nnue");
    require(!aetherz::generate_training_data(config, nullptr, &error).ok, "datagen fails without its network");
    require(error.find("could not load NNUE") == 0, "datagen reports the missing network");
}

void test_datagen_command_rejects_bad_options()
{
    std::ostringstream output;
    std::ostringstream error;
    require(aetherz::run_datagen_command({"datagen"}, output, error) == 1, "datagen requires an output file");
    require(
        aetherz::run_datagen_command({"datagen", temp_path("unused.txt"), "--nodes", "many"}, output, error) == 1,
        "datagen rejects non-numeric values");
    require(
        aetherz::run_datagen_command({"datagen", temp_path("unused.txt"), "--bogus", "1"}, output, error) == 1,
        "datagen rejects unknown options");
    require(error.str().find("usage: aetherz datagen") != std::string::npos, "datagen prints usage");
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

void test_bench_command_is_deterministic()
{
    auto run_bench = [] {
        std::ostringstream output;
        std::ostringstream error;
        const int status = aetherz::run_bench_command({"bench", "4"}, output, error);
        require(status == 0, "bench command succeeds");
        require(error.str().empty(), "bench command has no errors");

        const std::string text = output.str();
        const auto nodes_end = text.find(" nodes ");
        require(nodes_end != std::string::npos, "bench reports node count");
        const auto nodes_begin = text.rfind('\n', nodes_end) + 1;
        return text.substr(nodes_begin, nodes_end - nodes_begin);
    };

    require(run_bench() == run_bench(), "bench node count is deterministic");
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

void test_epd_test_command_reports_summary()
{
    const std::string path = temp_path("aetherz_epd_sample.epd");
    {
        std::ofstream output{path};
        output << "7k/8/5KQ1/8/8/8/8/8 w - - bm Qg7#; id \"mate.1\";\n";
    }

    std::ostringstream output;
    std::ostringstream error;

    const int status = aetherz::run_epd_test_command({"epdtest", path, "1"}, output, error);
    const std::string text = output.str();

    require(status == 0, "epdtest command succeeds");
    require(error.str().empty(), "epdtest command has no errors");
    require(text.find("epdtest summary tested 1 passed 1 failed 0") != std::string::npos, "epdtest reports passing summary");
}

} // namespace

int main()
{
    test_start_position_fen_round_trips();
    test_make_and_unmake_restores_state_exactly();
    test_null_move_restores_state_exactly();
    test_board_piece_cache_tracks_moves();
    test_incremental_hash_matches_full_hash();
    test_gives_check_matches_making_the_move();
    test_start_position_perft();
    test_kiwipete_perft();
    test_reference_perft_positions();
    test_uci_move_text_parses_promotions();
    test_pseudo_legal_tactical_generation_skips_quiet_moves();
    test_epd_parser_reads_best_avoid_and_id();
    test_epd_move_matching_accepts_san();
    test_iterative_search_returns_a_move();
    test_time_limited_search_returns_a_move();
    test_repeated_search_uses_transposition_table();
    test_search_thread_has_a_large_stack();
    test_search_object_fits_on_a_small_stack();
    test_multithreaded_search_finds_mate_and_stops();
    test_evaluation_trace_rebuilds_evaluation();
    test_evaluation_is_balanced_at_start();
    test_evaluation_rewards_material();
    test_piece_square_tables_reward_active_knight();
    test_attack_helpers_find_attackers();
    test_evaluation_rewards_bishop_pair();
    test_evaluation_rewards_passed_pawn();
    test_evaluation_rewards_rook_open_file();
    test_evaluation_rewards_supported_outpost();
    test_evaluation_prefers_castling_to_king_walk();
    test_static_exchange_evaluation_scores_winning_capture();
    test_static_exchange_evaluation_scores_losing_capture();
    test_search_finds_mate_in_one();
    test_search_prefers_castling_over_king_walk();
    test_search_scores_root_checkmate();
    test_search_scores_fifty_move_draw();
    test_search_scores_repetition_draw();
    test_uci_go_time_controls();
    test_nnue_loads_bullet_sized_files();
    test_nnue_parses_networks_from_memory();
    test_nnue_matches_hand_computed_evaluation();
    test_nnue_incremental_updates_match_refresh();
    test_nnue_evaluation_is_colour_symmetric();
    test_search_with_nnue_returns_legal_moves();
    test_uci_accepts_nnue_options();
    test_uci_infinite_search_waits_for_stop();
    test_search_stopped_at_once_returns_a_legal_move();
    test_uci_ignores_a_byte_order_mark();
    test_uci_depth_search_finishes_at_end_of_input();
    test_uci_reports_mate_scores();
    test_uci_ponder_waits_for_ponderhit();
    test_uci_movetime_uses_the_full_time();
    test_time_plan_stays_within_clock();
    test_datagen_writes_valid_bullet_text();
    test_datagen_is_independent_of_thread_count();
    test_datagen_plays_with_a_network();
    test_datagen_command_rejects_bad_options();
    test_search_benchmark_command_reports_summary();
    test_bench_command_is_deterministic();
    test_selfplay_command_reports_summary();
    test_tactics_command_passes_regressions();
    test_epd_test_command_reports_summary();

    std::cout << "engine tests passed\n";
    return 0;
}
