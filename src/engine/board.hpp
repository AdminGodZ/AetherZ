#pragma once

#include "engine/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

class Board {
public:
    static constexpr std::string_view start_fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    struct UndoState {
        std::array<Bitboard, 12> pieces{};
        Color side_to_move = Color::White;
        std::uint8_t castling_rights = 0;
        int en_passant_square = -1;
        int halfmove_clock = 0;
        int fullmove_number = 1;
        std::uint64_t hash = 0;
    };

    Board();

    [[nodiscard]] static Board startpos();
    [[nodiscard]] static std::optional<Board> from_fen(std::string_view fen);

    [[nodiscard]] bool set_fen(std::string_view fen);
    [[nodiscard]] std::string to_fen() const;

    [[nodiscard]] Color side_to_move() const { return side_to_move_; }
    [[nodiscard]] std::uint8_t castling_rights() const { return castling_rights_; }
    [[nodiscard]] int en_passant_square() const { return en_passant_square_; }
    [[nodiscard]] int halfmove_clock() const { return halfmove_clock_; }
    [[nodiscard]] int fullmove_number() const { return fullmove_number_; }
    [[nodiscard]] std::uint64_t hash() const { return hash_; }

    [[nodiscard]] Bitboard pieces(Piece piece) const;
    [[nodiscard]] Bitboard pieces(Color color, PieceType type) const;
    [[nodiscard]] Bitboard occupancy(Color color) const;
    [[nodiscard]] Bitboard occupancy() const;
    [[nodiscard]] Piece piece_at(int square) const;
    [[nodiscard]] int king_square(Color color) const;

    [[nodiscard]] bool is_square_attacked(int square, Color by_color) const;
    [[nodiscard]] bool in_check(Color color) const;

    [[nodiscard]] std::vector<Move> generate_pseudo_legal_moves() const;
    [[nodiscard]] std::vector<Move> generate_legal_moves() const;

    [[nodiscard]] bool make_move(const Move& move, UndoState& undo);
    void unmake_move(const UndoState& undo);

private:
    enum Castling : std::uint8_t {
        WhiteKingSide = 1 << 0,
        WhiteQueenSide = 1 << 1,
        BlackKingSide = 1 << 2,
        BlackQueenSide = 1 << 3,
    };

    std::array<Bitboard, 12> pieces_{};
    Color side_to_move_ = Color::White;
    std::uint8_t castling_rights_ = 0;
    int en_passant_square_ = -1;
    int halfmove_clock_ = 0;
    int fullmove_number_ = 1;
    std::uint64_t hash_ = 0;

    void clear();
    void put_piece(Piece piece, int square);
    void remove_piece(Piece piece, int square);
    void clear_square(int square);
    void refresh_hash();
    [[nodiscard]] std::uint64_t compute_hash() const;
};

} // namespace aetherz
