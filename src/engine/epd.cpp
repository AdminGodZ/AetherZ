#include "engine/epd.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace aetherz {
namespace {

std::string trim(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string{value.substr(begin, end - begin)};
}

std::vector<std::string> split_words(std::string_view value)
{
    std::istringstream stream{std::string{value}};
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

std::string unquote(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string clean_move_token(std::string_view value)
{
    std::string token = trim(value);
    while (!token.empty() && (token.back() == '!' || token.back() == '?' || token.back() == '+' || token.back() == '#')) {
        token.pop_back();
    }

    const auto ep = token.find("e.p.");
    if (ep != std::string::npos) {
        token.erase(ep);
        token = trim(token);
    }

    return token;
}

std::optional<PieceType> san_piece_type(char value)
{
    switch (value) {
    case 'K':
        return PieceType::King;
    case 'Q':
        return PieceType::Queen;
    case 'R':
        return PieceType::Rook;
    case 'B':
        return PieceType::Bishop;
    case 'N':
        return PieceType::Knight;
    default:
        return std::nullopt;
    }
}

bool same_move(const Move& left, const Move& right)
{
    return left.from == right.from
        && left.to == right.to
        && left.promotion == right.promotion;
}

bool matches_uci(const Move& move, std::string_view expected)
{
    const auto parsed = move_from_uci(expected);
    return parsed && same_move(move, *parsed);
}

bool matches_castling(const Move& move, std::string_view expected)
{
    return ((expected == "O-O" || expected == "0-0") && move.flag == MoveFlag::KingCastle)
        || ((expected == "O-O-O" || expected == "0-0-0") && move.flag == MoveFlag::QueenCastle);
}

bool matches_san(const Board& board, const Move& move, std::string token)
{
    token = clean_move_token(token);
    if (token.empty()) {
        return false;
    }
    if (matches_castling(move, token)) {
        return true;
    }

    PieceType expected_type = PieceType::Pawn;
    if (const auto piece = san_piece_type(token.front())) {
        expected_type = *piece;
        token.erase(token.begin());
    }

    PieceType expected_promotion = PieceType::None;
    if (const auto promotion_marker = token.find('='); promotion_marker != std::string::npos) {
        if (promotion_marker + 1 >= token.size()) {
            return false;
        }
        const auto promotion = san_piece_type(token[promotion_marker + 1]);
        if (!promotion) {
            return false;
        }
        expected_promotion = *promotion;
        token.erase(promotion_marker);
    }

    std::string compact;
    compact.reserve(token.size());
    for (const char value : token) {
        if (value != 'x' && value != '-' && value != ':') {
            compact.push_back(value);
        }
    }

    if (compact.size() < 2) {
        return false;
    }

    const std::string target_text = compact.substr(compact.size() - 2);
    const auto target = square_from_string(target_text);
    if (!target || move.to != *target) {
        return false;
    }

    const Piece moving = board.piece_at(move.from);
    if (moving == Piece::None || piece_type(moving) != expected_type) {
        return false;
    }
    if (expected_promotion != PieceType::None && move.promotion != expected_promotion) {
        return false;
    }

    const std::string disambiguation = compact.substr(0, compact.size() - 2);
    if (disambiguation.empty()) {
        return true;
    }
    if (disambiguation.size() > 2) {
        return false;
    }

    for (const char value : disambiguation) {
        if (value >= 'a' && value <= 'h' && file_of(move.from) != value - 'a') {
            return false;
        }
        if (value >= '1' && value <= '8' && rank_of(move.from) != value - '1') {
            return false;
        }
    }

    return true;
}

void parse_operation(EpdPosition& position, std::string_view operation)
{
    const std::string text = trim(operation);
    if (text.empty()) {
        return;
    }

    const auto space = text.find_first_of(" \t");
    const std::string opcode = space == std::string::npos ? text : text.substr(0, space);
    const std::string value = space == std::string::npos ? "" : trim(std::string_view{text}.substr(space + 1));

    if (opcode == "bm") {
        position.best_moves = split_words(value);
    } else if (opcode == "am") {
        position.avoid_moves = split_words(value);
    } else if (opcode == "id" || (opcode == "od" && position.id.empty())) {
        position.id = unquote(value);
    }
}

} // namespace

std::optional<EpdPosition> parse_epd(std::string_view text)
{
    const std::string line = trim(text);
    if (line.empty() || line.front() == '#') {
        return std::nullopt;
    }

    std::istringstream stream{line};
    std::string board;
    std::string side;
    std::string castling;
    std::string en_passant;
    if (!(stream >> board >> side >> castling >> en_passant)) {
        return std::nullopt;
    }

    EpdPosition position;
    position.fen = board + ' ' + side + ' ' + castling + ' ' + en_passant + " 0 1";

    std::string operations;
    std::getline(stream, operations);

    std::size_t begin = 0;
    while (begin < operations.size()) {
        const std::size_t end = operations.find(';', begin);
        parse_operation(position, std::string_view{operations}.substr(begin, end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    return position;
}

std::vector<EpdPosition> load_epd_file(std::string_view path)
{
    std::ifstream input{std::string{path}};
    std::vector<EpdPosition> positions;

    std::string line;
    while (std::getline(input, line)) {
        if (auto position = parse_epd(line)) {
            positions.push_back(*position);
        }
    }

    return positions;
}

bool epd_move_matches(const Board& board, std::string_view expected, const Move& move)
{
    const std::string token = clean_move_token(expected);
    return matches_uci(move, token) || matches_san(board, move, token);
}

} // namespace aetherz
