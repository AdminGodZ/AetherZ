#include "engine/types.hpp"

#include <array>
#include <cctype>

namespace aetherz {

bool Move::is_valid() const
{
    return is_valid_square(from) && is_valid_square(to) && from != to;
}

Piece make_piece(Color color, PieceType type)
{
    if (type == PieceType::None) {
        return Piece::None;
    }

    const int offset = color == Color::White ? 1 : 7;
    return static_cast<Piece>(offset + piece_type_index(type));
}

Color piece_color(Piece piece)
{
    const int value = static_cast<int>(piece);
    return value >= static_cast<int>(Piece::BlackPawn) ? Color::Black : Color::White;
}

PieceType piece_type(Piece piece)
{
    if (piece == Piece::None) {
        return PieceType::None;
    }

    const int value = static_cast<int>(piece);
    return static_cast<PieceType>((value - 1) % 6);
}

int piece_index(Piece piece)
{
    return static_cast<int>(piece) - 1;
}

char piece_to_fen(Piece piece)
{
    switch (piece) {
    case Piece::WhitePawn:
        return 'P';
    case Piece::WhiteKnight:
        return 'N';
    case Piece::WhiteBishop:
        return 'B';
    case Piece::WhiteRook:
        return 'R';
    case Piece::WhiteQueen:
        return 'Q';
    case Piece::WhiteKing:
        return 'K';
    case Piece::BlackPawn:
        return 'p';
    case Piece::BlackKnight:
        return 'n';
    case Piece::BlackBishop:
        return 'b';
    case Piece::BlackRook:
        return 'r';
    case Piece::BlackQueen:
        return 'q';
    case Piece::BlackKing:
        return 'k';
    case Piece::None:
        return ' ';
    }

    return ' ';
}

std::optional<Piece> piece_from_fen(char value)
{
    switch (value) {
    case 'P':
        return Piece::WhitePawn;
    case 'N':
        return Piece::WhiteKnight;
    case 'B':
        return Piece::WhiteBishop;
    case 'R':
        return Piece::WhiteRook;
    case 'Q':
        return Piece::WhiteQueen;
    case 'K':
        return Piece::WhiteKing;
    case 'p':
        return Piece::BlackPawn;
    case 'n':
        return Piece::BlackKnight;
    case 'b':
        return Piece::BlackBishop;
    case 'r':
        return Piece::BlackRook;
    case 'q':
        return Piece::BlackQueen;
    case 'k':
        return Piece::BlackKing;
    default:
        return std::nullopt;
    }
}

std::string square_to_string(int square)
{
    if (!is_valid_square(square)) {
        return "-";
    }

    std::string result;
    result.push_back(static_cast<char>('a' + file_of(square)));
    result.push_back(static_cast<char>('1' + rank_of(square)));
    return result;
}

std::optional<int> square_from_string(std::string_view value)
{
    if (value.size() != 2) {
        return std::nullopt;
    }

    const char file = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
    const char rank = value[1];
    if (file < 'a' || file > 'h' || rank < '1' || rank > '8') {
        return std::nullopt;
    }

    return (rank - '1') * 8 + (file - 'a');
}

char promotion_to_char(PieceType type)
{
    switch (type) {
    case PieceType::Knight:
        return 'n';
    case PieceType::Bishop:
        return 'b';
    case PieceType::Rook:
        return 'r';
    case PieceType::Queen:
        return 'q';
    case PieceType::Pawn:
    case PieceType::King:
    case PieceType::None:
        return '\0';
    }

    return '\0';
}

std::optional<PieceType> promotion_from_char(char value)
{
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(value)))) {
    case 'n':
        return PieceType::Knight;
    case 'b':
        return PieceType::Bishop;
    case 'r':
        return PieceType::Rook;
    case 'q':
        return PieceType::Queen;
    default:
        return std::nullopt;
    }
}

std::string move_to_uci(const Move& move)
{
    if (!move.is_valid()) {
        return "0000";
    }

    std::string result = square_to_string(move.from) + square_to_string(move.to);
    const char promotion = promotion_to_char(move.promotion);
    if (promotion != '\0') {
        result.push_back(promotion);
    }

    return result;
}

std::optional<Move> move_from_uci(std::string_view value)
{
    if (value.size() != 4 && value.size() != 5) {
        return std::nullopt;
    }

    const auto from = square_from_string(value.substr(0, 2));
    const auto to = square_from_string(value.substr(2, 2));
    if (!from || !to) {
        return std::nullopt;
    }

    Move move{.from = *from, .to = *to};
    if (value.size() == 5) {
        const auto promotion = promotion_from_char(value[4]);
        if (!promotion) {
            return std::nullopt;
        }
        move.promotion = *promotion;
    }

    return move;
}

} // namespace aetherz
