#include "engine/benchmark.hpp"
#include "engine/attacks.hpp"
#include "engine/board.hpp"
#include "engine/epd.hpp"
#include "engine/evaluate.hpp"
#include "engine/nnue/nnue.hpp"
#include "engine/perft.hpp"
#include "engine/search.hpp"
#include "engine/see.hpp"
#include "engine/types.hpp"
#include "engine/uci.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

    // At very low depth developing moves such as Ne5 are just as good, so
    // search deep enough for king safety to decide.
    aetherz::Search search;
    const auto result = search.find_best_move(*board, 8);

    require(result.best_move.has_value(), "castling search finds a move");
    const std::string best = aetherz::move_to_uci(*result.best_move);
    require(best == "e1g1" || best == "e1c1", "search castles instead of walking the king");
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

void test_nnue_placeholder_loads_and_falls_back_to_classical_eval()
{
    const std::string path = "/tmp/aetherz_dummy.nnue";
    {
        std::ofstream output{path};
        output << "AETHERZ_NNUE_V1\n";
        output << "name=test-placeholder\n";
        output << "feature_set=halfka-v1\n";
        output << "accumulator_size=768\n";
        output << "hidden_size=32\n";
    }

    aetherz::nnue::NnueEvaluator evaluator;
    std::string error;
    require(evaluator.load(path, &error), "placeholder NNUE file loads");
    require(error.empty(), "placeholder NNUE load has no error");
    require(evaluator.loaded(), "placeholder NNUE reports loaded");
    require(evaluator.metadata().name == "test-placeholder", "placeholder NNUE metadata loads");

    auto board = aetherz::Board::startpos();
    evaluator.refresh(board);
    require(evaluator.evaluate(board) == aetherz::evaluate_side_to_move(board), "placeholder NNUE eval falls back to classical eval");
}

void test_nnue_tiny_network_uses_halfka_feature()
{
    const std::string path = "/tmp/aetherz_tiny_feature.nnue";
    const int feature = aetherz::nnue::halfka_feature_index(
        aetherz::Color::White,
        4,
        aetherz::Piece::WhiteQueen,
        0);
    {
        std::ofstream output{path};
        output << "AETHERZ_NNUE_V1\n";
        output << "name=tiny-feature\n";
        output << "feature_set=halfka-v1\n";
        output << "accumulator_size=2\n";
        output << "hidden_size=1\n";
        output << "input_scale=1\n";
        output << "hidden_scale=1\n";
        output << "output_scale=1\n";
        output << "accumulator_bias 0 0\n";
        output << "feature " << feature << " 32 0\n";
        output << "hidden1_bias 0\n";
        output << "hidden1_weight 0 1\n";
        output << "hidden2_bias 0\n";
        output << "hidden2_weight 0 1\n";
        output << "output_bias 0\n";
        output << "output_weight 1\n";
    }

    auto active = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 0 1");
    auto inactive = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(active.has_value(), "active tiny NNUE FEN parses");
    require(inactive.has_value(), "inactive tiny NNUE FEN parses");

    aetherz::nnue::NnueEvaluator evaluator;
    require(evaluator.load(path), "tiny NNUE file loads");
    require(evaluator.ready(), "tiny NNUE file is inference-ready");
    evaluator.refresh(*active);

    require(evaluator.evaluate(*active) == 32, "tiny NNUE sees active HalfKA feature");
    require(evaluator.evaluate(*inactive) == 0, "tiny NNUE ignores missing HalfKA feature");
}

void test_nnue_incremental_push_matches_refresh()
{
    const std::string path = "/tmp/aetherz_tiny_incremental.nnue";
    const int feature = aetherz::nnue::halfka_feature_index(
        aetherz::Color::White,
        4,
        aetherz::Piece::WhiteQueen,
        0);
    {
        std::ofstream output{path};
        output << "AETHERZ_NNUE_V1\n";
        output << "name=tiny-incremental\n";
        output << "feature_set=halfka-v1\n";
        output << "accumulator_size=2\n";
        output << "hidden_size=1\n";
        output << "accumulator_bias 0 0\n";
        output << "feature " << feature << " 32 0\n";
        output << "hidden1_bias 0\n";
        output << "hidden1_weight 0 1\n";
        output << "hidden2_bias 0\n";
        output << "hidden2_weight 0 1\n";
        output << "output_bias 0\n";
        output << "output_weight 1\n";
    }

    auto board = aetherz::Board::from_fen("4k3/8/8/8/8/8/8/Q3K3 w - - 0 1");
    require(board.has_value(), "incremental tiny NNUE FEN parses");

    aetherz::nnue::NnueEvaluator evaluator;
    require(evaluator.load(path), "incremental tiny NNUE file loads");
    evaluator.refresh(*board);
    require(evaluator.evaluate(*board) == 32, "incremental NNUE starts from active feature");

    const auto move = require_legal_move(*board, "a1a2");
    aetherz::Board::UndoState undo;
    require(board->make_move(move, undo), "incremental NNUE test move can be made");
    evaluator.push(*board, aetherz::nnue::dirty_move_from(*board, undo));
    require(evaluator.evaluate(*board) == 0, "incremental NNUE removes moved feature");

    evaluator.pop();
    board->unmake_move(undo);
    require(evaluator.evaluate(*board) == 32, "incremental NNUE pop restores previous accumulator");
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
    const std::string path = "/tmp/aetherz_uci_dummy.nnue";
    {
        std::ofstream output{path};
        output << "AETHERZ_NNUE_V1\n";
        output << "name=uci-placeholder\n";
    }

    std::istringstream input{
        "uci\n"
        "setoption name UseNNUE value true\n"
        "setoption name EvalFile value /tmp/aetherz_uci_dummy.nnue\n"
        "isready\n"
        "quit\n"};
    std::ostringstream output;

    aetherz::UciEngine engine;
    engine.loop(input, output);

    const std::string text = output.str();
    require(text.find("option name UseNNUE") != std::string::npos, "UCI exposes UseNNUE");
    require(text.find("option name EvalFile") != std::string::npos, "UCI exposes EvalFile");
    require(text.find("info string loaded NNUE /tmp/aetherz_uci_dummy.nnue") != std::string::npos, "UCI loads placeholder NNUE");
    require(text.find("readyok") != std::string::npos, "UCI remains ready after NNUE load");
}

void test_training_data_command_writes_bullet_text()
{
    const std::string path = "/tmp/aetherz_train_sample.txt";
    std::ostringstream output;
    std::ostringstream error;

    const int status = aetherz::run_training_data_command({"traindata", path, "4", "1", "8"}, output, error);
    require(status == 0, "training data command succeeds");
    require(error.str().empty(), "training data command has no errors");

    std::ifstream input{path};
    std::string line;
    int lines = 0;
    while (std::getline(input, line)) {
        require(line.find(" | ") != std::string::npos, "training data line uses Bullet text separators");
        ++lines;
    }
    require(lines == 4, "training data command writes requested positions");
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
    const std::string path = "/tmp/aetherz_epd_sample.epd";
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
    test_nnue_placeholder_loads_and_falls_back_to_classical_eval();
    test_nnue_tiny_network_uses_halfka_feature();
    test_nnue_incremental_push_matches_refresh();
    test_uci_accepts_nnue_options();
    test_uci_infinite_search_waits_for_stop();
    test_uci_depth_search_finishes_at_end_of_input();
    test_uci_reports_mate_scores();
    test_uci_ponder_waits_for_ponderhit();
    test_time_plan_stays_within_clock();
    test_training_data_command_writes_bullet_text();
    test_search_benchmark_command_reports_summary();
    test_bench_command_is_deterministic();
    test_selfplay_command_reports_summary();
    test_tactics_command_passes_regressions();
    test_epd_test_command_reports_summary();

    std::cout << "engine tests passed\n";
    return 0;
}
