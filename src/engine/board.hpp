#pragma once

#include "engine/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aetherz {

// Fixed-capacity move container so move generation never allocates.
class MoveList {
public:
    static constexpr std::size_t capacity = 256;

    void push_back(const Move& move) { moves_[size_++] = move; }
    void clear() { size_ = 0; }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] Move& operator[](std::size_t index) { return moves_[index]; }
    [[nodiscard]] const Move& operator[](std::size_t index) const { return moves_[index]; }
    [[nodiscard]] Move* begin() { return moves_.data(); }
    [[nodiscard]] Move* end() { return moves_.data() + size_; }
    [[nodiscard]] const Move* begin() const { return moves_.data(); }
    [[nodiscard]] const Move* end() const { return moves_.data() + size_; }

private:
    std::array<Move, capacity> moves_;
    std::size_t size_ = 0;
};

class Board {
public:
    static constexpr std::string_view start_fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    // Everything needed to take back a move (or null move).
    struct UndoState {
        Move move;
        Piece moved = Piece::None;
        Piece captured = Piece::None;
        int captured_square = -1;
        bool null_move = false;
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
    [[nodiscard]] bool can_castle(Color color, bool kingside) const;
    [[nodiscard]] const std::array<Piece, 64>& squares() const { return squares_; }

    [[nodiscard]] Bitboard pieces(Piece piece) const { return piece == Piece::None ? 0 : pieces_[piece_index(piece)]; }
    [[nodiscard]] Bitboard pieces(Color color, PieceType type) const
    {
        return type == PieceType::None ? 0 : pieces_[color_index(color) * 6 + piece_type_index(type)];
    }
    [[nodiscard]] Bitboard occupancy(Color color) const { return occupancies_[color_index(color)]; }
    [[nodiscard]] Bitboard occupancy() const { return occupancy_; }
    [[nodiscard]] Piece piece_at(int square) const { return is_valid_square(square) ? squares_[square] : Piece::None; }
    [[nodiscard]] int king_square(Color color) const { return king_squares_[color_index(color)]; }

    [[nodiscard]] bool is_square_attacked(int square, Color by_color) const;
    [[nodiscard]] bool in_check(Color color) const;

    [[nodiscard]] MoveList generate_pseudo_legal_moves() const;
    [[nodiscard]] MoveList generate_pseudo_legal_tactical_moves() const;
    [[nodiscard]] MoveList generate_legal_moves() const;

    [[nodiscard]] bool make_move(const Move& move, UndoState& undo);
    [[nodiscard]] bool make_null_move(UndoState& undo);
    void unmake_move(const UndoState& undo);

private:
    enum Castling : std::uint8_t {
        WhiteKingSide = 1 << 0,
        WhiteQueenSide = 1 << 1,
        BlackKingSide = 1 << 2,
        BlackQueenSide = 1 << 3,
    };

    std::array<Bitboard, 12> pieces_{};
    std::array<Piece, 64> squares_{};
    std::array<Bitboard, 2> occupancies_{};
    Bitboard occupancy_ = 0;
    std::array<int, 2> king_squares_{};
    Color side_to_move_ = Color::White;
    std::uint8_t castling_rights_ = 0;
    int en_passant_square_ = -1;
    int halfmove_clock_ = 0;
    int fullmove_number_ = 1;
    std::uint64_t hash_ = 0;

    void clear();
    void put_piece(Piece piece, int square);
    void clear_square(int square);
    void move_piece(Piece piece, int from, int to);
    void refresh_hash();
    [[nodiscard]] std::uint64_t compute_hash() const;
    template <bool TacticalOnly>
    void generate_moves(MoveList& moves) const;
};

} // namespace aetherz
