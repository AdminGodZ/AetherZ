#include "engine/board.hpp"
#include "engine/perft.hpp"
#include "engine/types.hpp"

#include <cstdlib>
#include <iostream>
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

} // namespace

int main()
{
    test_start_position_fen_round_trips();
    test_make_and_unmake_restores_state_exactly();
    test_start_position_perft();
    test_kiwipete_perft();
    test_uci_move_text_parses_promotions();

    std::cout << "engine tests passed\n";
    return 0;
}
