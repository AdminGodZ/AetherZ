#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aetherz {

using Bitboard = std::uint64_t;

enum class Color : std::uint8_t {
    White = 0,
    Black = 1,
};

enum class PieceType : std::uint8_t {
    Pawn = 0,
    Knight = 1,
    Bishop = 2,
    Rook = 3,
    Queen = 4,
    King = 5,
    None = 6,
};

enum class Piece : std::uint8_t {
    None = 0,
    WhitePawn,
    WhiteKnight,
    WhiteBishop,
    WhiteRook,
    WhiteQueen,
    WhiteKing,
    BlackPawn,
    BlackKnight,
    BlackBishop,
    BlackRook,
    BlackQueen,
    BlackKing,
};

enum class MoveFlag : std::uint8_t {
    Quiet,
    DoublePawnPush,
    KingCastle,
    QueenCastle,
    Capture,
    EnPassant,
    Promotion,
    PromotionCapture,
};

struct Move {
    int from = -1;
    int to = -1;
    PieceType promotion = PieceType::None;
    MoveFlag flag = MoveFlag::Quiet;

    [[nodiscard]] bool is_valid() const;
};

[[nodiscard]] constexpr Color opposite(Color color)
{
    return color == Color::White ? Color::Black : Color::White;
}

[[nodiscard]] constexpr int color_index(Color color)
{
    return static_cast<int>(color);
}

[[nodiscard]] constexpr int piece_type_index(PieceType type)
{
    return static_cast<int>(type);
}

[[nodiscard]] constexpr bool is_valid_square(int square)
{
    return square >= 0 && square < 64;
}

[[nodiscard]] constexpr int file_of(int square)
{
    return square & 7;
}

[[nodiscard]] constexpr int rank_of(int square)
{
    return square >> 3;
}

[[nodiscard]] constexpr Bitboard square_bb(int square)
{
    return Bitboard{1} << square;
}

[[nodiscard]] Piece make_piece(Color color, PieceType type);
[[nodiscard]] Color piece_color(Piece piece);
[[nodiscard]] PieceType piece_type(Piece piece);
[[nodiscard]] int piece_index(Piece piece);
[[nodiscard]] char piece_to_fen(Piece piece);
[[nodiscard]] std::optional<Piece> piece_from_fen(char value);

[[nodiscard]] std::string square_to_string(int square);
[[nodiscard]] std::optional<int> square_from_string(std::string_view value);
[[nodiscard]] char promotion_to_char(PieceType type);
[[nodiscard]] std::optional<PieceType> promotion_from_char(char value);
[[nodiscard]] std::string move_to_uci(const Move& move);
[[nodiscard]] std::optional<Move> move_from_uci(std::string_view value);

} // namespace aetherz
