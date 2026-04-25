#include "engine/board.hpp"

#include <array>
#include <bit>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

namespace aetherz {
namespace {

constexpr std::array<PieceType, 4> promotion_pieces{
    PieceType::Queen,
    PieceType::Rook,
    PieceType::Bishop,
    PieceType::Knight,
};

constexpr std::array<std::pair<int, int>, 4> bishop_directions{{
    {1, 1},
    {-1, 1},
    {1, -1},
    {-1, -1},
}};

constexpr std::array<std::pair<int, int>, 4> rook_directions{{
    {1, 0},
    {-1, 0},
    {0, 1},
    {0, -1},
}};

constexpr std::array<std::pair<int, int>, 8> knight_offsets{{
    {1, 2},
    {2, 1},
    {2, -1},
    {1, -2},
    {-1, -2},
    {-2, -1},
    {-2, 1},
    {-1, 2},
}};

constexpr std::array<std::pair<int, int>, 8> king_offsets{{
    {1, 1},
    {1, 0},
    {1, -1},
    {0, -1},
    {-1, -1},
    {-1, 0},
    {-1, 1},
    {0, 1},
}};

struct ZobristKeys {
    std::array<std::array<std::uint64_t, 64>, 12> pieces{};
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 8> en_passant_file{};
    std::uint64_t side_to_move = 0;
};

std::uint64_t splitmix64(std::uint64_t& state)
{
    std::uint64_t result = (state += 0x9e3779b97f4a7c15ULL);
    result = (result ^ (result >> 30)) * 0xbf58476d1ce4e5b9ULL;
    result = (result ^ (result >> 27)) * 0x94d049bb133111ebULL;
    return result ^ (result >> 31);
}

const ZobristKeys& zobrist_keys()
{
    static const ZobristKeys keys = [] {
        ZobristKeys generated;
        std::uint64_t seed = 0x4145544845525aULL;

        for (auto& piece_keys : generated.pieces) {
            for (auto& key : piece_keys) {
                key = splitmix64(seed);
            }
        }

        for (auto& key : generated.castling) {
            key = splitmix64(seed);
        }

        for (auto& key : generated.en_passant_file) {
            key = splitmix64(seed);
        }

        generated.side_to_move = splitmix64(seed);
        return generated;
    }();

    return keys;
}

int pop_lsb(Bitboard& bitboard)
{
    const int square = std::countr_zero(bitboard);
    bitboard &= bitboard - 1;
    return square;
}

bool contains_piece(const Board& board, int square, Piece piece)
{
    return is_valid_square(square) && board.piece_at(square) == piece;
}

void add_promotion_moves(
    std::vector<Move>& moves,
    int from,
    int to,
    MoveFlag flag)
{
    for (const PieceType promotion : promotion_pieces) {
        moves.push_back(Move{.from = from, .to = to, .promotion = promotion, .flag = flag});
    }
}

void add_slider_moves(
    const Board& board,
    std::vector<Move>& moves,
    int from,
    Color us,
    const std::array<std::pair<int, int>, 4>& directions)
{
    for (const auto& [delta_file, delta_rank] : directions) {
        int file = file_of(from) + delta_file;
        int rank = rank_of(from) + delta_rank;

        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const int to = rank * 8 + file;
            const Piece target = board.piece_at(to);
            if (target == Piece::None) {
                moves.push_back(Move{.from = from, .to = to});
            } else {
                if (piece_color(target) != us && piece_type(target) != PieceType::King) {
                    moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::Capture});
                }
                break;
            }

            file += delta_file;
            rank += delta_rank;
        }
    }
}

bool attacks_on_ray(
    const Board& board,
    int square,
    Color by_color,
    const std::array<std::pair<int, int>, 4>& directions,
    PieceType primary_slider)
{
    for (const auto& [delta_file, delta_rank] : directions) {
        int file = file_of(square) + delta_file;
        int rank = rank_of(square) + delta_rank;

        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const Piece piece = board.piece_at(rank * 8 + file);
            if (piece == Piece::None) {
                file += delta_file;
                rank += delta_rank;
                continue;
            }

            if (piece_color(piece) != by_color) {
                break;
            }

            const PieceType type = piece_type(piece);
            if (type == primary_slider || type == PieceType::Queen) {
                return true;
            }
            break;
        }
    }

    return false;
}

} // namespace

Board::Board()
{
    clear();
}

Board Board::startpos()
{
    auto board = from_fen(start_fen);
    return board.value_or(Board{});
}

std::optional<Board> Board::from_fen(std::string_view fen)
{
    Board board;
    if (!board.set_fen(fen)) {
        return std::nullopt;
    }

    return board;
}

bool Board::set_fen(std::string_view fen)
{
    Board parsed;
    parsed.clear();

    std::istringstream stream{std::string{fen}};
    std::string board_part;
    std::string side_part;
    std::string castling_part;
    std::string en_passant_part;

    if (!(stream >> board_part >> side_part >> castling_part >> en_passant_part)) {
        return false;
    }

    int rank = 7;
    int file = 0;
    int white_kings = 0;
    int black_kings = 0;

    for (const char value : board_part) {
        if (value == '/') {
            if (file != 8 || rank == 0) {
                return false;
            }
            --rank;
            file = 0;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(value))) {
            const int empty = value - '0';
            if (empty <= 0 || file + empty > 8) {
                return false;
            }
            file += empty;
            continue;
        }

        const auto piece = piece_from_fen(value);
        if (!piece || file >= 8 || rank < 0) {
            return false;
        }

        if (*piece == Piece::WhiteKing) {
            ++white_kings;
        } else if (*piece == Piece::BlackKing) {
            ++black_kings;
        }

        parsed.put_piece(*piece, rank * 8 + file);
        ++file;
    }

    if (rank != 0 || file != 8 || white_kings != 1 || black_kings != 1) {
        return false;
    }

    if (side_part == "w") {
        parsed.side_to_move_ = Color::White;
    } else if (side_part == "b") {
        parsed.side_to_move_ = Color::Black;
    } else {
        return false;
    }

    if (castling_part != "-") {
        for (const char value : castling_part) {
            switch (value) {
            case 'K':
                parsed.castling_rights_ |= WhiteKingSide;
                break;
            case 'Q':
                parsed.castling_rights_ |= WhiteQueenSide;
                break;
            case 'k':
                parsed.castling_rights_ |= BlackKingSide;
                break;
            case 'q':
                parsed.castling_rights_ |= BlackQueenSide;
                break;
            default:
                return false;
            }
        }
    }

    if (en_passant_part == "-") {
        parsed.en_passant_square_ = -1;
    } else {
        const auto square = square_from_string(en_passant_part);
        if (!square) {
            return false;
        }
        parsed.en_passant_square_ = *square;
    }

    if (!(stream >> parsed.halfmove_clock_)) {
        parsed.halfmove_clock_ = 0;
    }

    if (!(stream >> parsed.fullmove_number_)) {
        parsed.fullmove_number_ = 1;
    }

    if (parsed.halfmove_clock_ < 0 || parsed.fullmove_number_ <= 0) {
        return false;
    }

    parsed.refresh_hash();
    *this = parsed;
    return true;
}

std::string Board::to_fen() const
{
    std::string result;

    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const Piece piece = piece_at(rank * 8 + file);
            if (piece == Piece::None) {
                ++empty;
                continue;
            }

            if (empty > 0) {
                result.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            result.push_back(piece_to_fen(piece));
        }

        if (empty > 0) {
            result.push_back(static_cast<char>('0' + empty));
        }

        if (rank > 0) {
            result.push_back('/');
        }
    }

    result += side_to_move_ == Color::White ? " w " : " b ";

    std::string castling;
    if ((castling_rights_ & WhiteKingSide) != 0) {
        castling.push_back('K');
    }
    if ((castling_rights_ & WhiteQueenSide) != 0) {
        castling.push_back('Q');
    }
    if ((castling_rights_ & BlackKingSide) != 0) {
        castling.push_back('k');
    }
    if ((castling_rights_ & BlackQueenSide) != 0) {
        castling.push_back('q');
    }
    result += castling.empty() ? "-" : castling;
    result.push_back(' ');
    result += en_passant_square_ == -1 ? "-" : square_to_string(en_passant_square_);
    result.push_back(' ');
    result += std::to_string(halfmove_clock_);
    result.push_back(' ');
    result += std::to_string(fullmove_number_);
    return result;
}

Bitboard Board::pieces(Piece piece) const
{
    if (piece == Piece::None) {
        return 0;
    }

    return pieces_[piece_index(piece)];
}

Bitboard Board::pieces(Color color, PieceType type) const
{
    return pieces(make_piece(color, type));
}

Bitboard Board::occupancy(Color color) const
{
    Bitboard result = 0;
    for (int type = 0; type < 6; ++type) {
        result |= pieces(make_piece(color, static_cast<PieceType>(type)));
    }
    return result;
}

Bitboard Board::occupancy() const
{
    return occupancy(Color::White) | occupancy(Color::Black);
}

Piece Board::piece_at(int square) const
{
    if (!is_valid_square(square)) {
        return Piece::None;
    }

    const Bitboard mask = square_bb(square);
    for (int index = 0; index < 12; ++index) {
        if ((pieces_[index] & mask) != 0) {
            return static_cast<Piece>(index + 1);
        }
    }

    return Piece::None;
}

int Board::king_square(Color color) const
{
    const Bitboard kings = pieces(color, PieceType::King);
    if (kings == 0) {
        return -1;
    }

    return std::countr_zero(kings);
}

bool Board::is_square_attacked(int square, Color by_color) const
{
    if (!is_valid_square(square)) {
        return false;
    }

    const int file = file_of(square);

    if (by_color == Color::White) {
        if (file < 7 && contains_piece(*this, square - 7, Piece::WhitePawn)) {
            return true;
        }
        if (file > 0 && contains_piece(*this, square - 9, Piece::WhitePawn)) {
            return true;
        }
    } else {
        if (file > 0 && contains_piece(*this, square + 7, Piece::BlackPawn)) {
            return true;
        }
        if (file < 7 && contains_piece(*this, square + 9, Piece::BlackPawn)) {
            return true;
        }
    }

    for (const auto& [delta_file, delta_rank] : knight_offsets) {
        const int target_file = file + delta_file;
        const int target_rank = rank_of(square) + delta_rank;
        if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) {
            continue;
        }

        if (piece_at(target_rank * 8 + target_file) == make_piece(by_color, PieceType::Knight)) {
            return true;
        }
    }

    if (attacks_on_ray(*this, square, by_color, bishop_directions, PieceType::Bishop)) {
        return true;
    }
    if (attacks_on_ray(*this, square, by_color, rook_directions, PieceType::Rook)) {
        return true;
    }

    for (const auto& [delta_file, delta_rank] : king_offsets) {
        const int target_file = file + delta_file;
        const int target_rank = rank_of(square) + delta_rank;
        if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) {
            continue;
        }

        if (piece_at(target_rank * 8 + target_file) == make_piece(by_color, PieceType::King)) {
            return true;
        }
    }

    return false;
}

bool Board::in_check(Color color) const
{
    const int king = king_square(color);
    return is_valid_square(king) && is_square_attacked(king, opposite(color));
}

std::vector<Move> Board::generate_pseudo_legal_moves() const
{
    std::vector<Move> moves;
    moves.reserve(128);

    const Color us = side_to_move_;
    const Color them = opposite(us);
    const int forward = us == Color::White ? 8 : -8;
    const int start_rank = us == Color::White ? 1 : 6;
    const int promotion_rank = us == Color::White ? 6 : 1;

    Bitboard pawns = pieces(us, PieceType::Pawn);
    while (pawns != 0) {
        const int from = pop_lsb(pawns);
        const int from_file = file_of(from);
        const int from_rank = rank_of(from);
        const int one_forward = from + forward;

        if (is_valid_square(one_forward) && piece_at(one_forward) == Piece::None) {
            if (from_rank == promotion_rank) {
                add_promotion_moves(moves, from, one_forward, MoveFlag::Promotion);
            } else {
                moves.push_back(Move{.from = from, .to = one_forward});
                const int two_forward = from + 2 * forward;
                if (from_rank == start_rank && piece_at(two_forward) == Piece::None) {
                    moves.push_back(Move{
                        .from = from,
                        .to = two_forward,
                        .flag = MoveFlag::DoublePawnPush,
                    });
                }
            }
        }

        const std::array<int, 2> capture_offsets = us == Color::White
            ? std::array<int, 2>{7, 9}
            : std::array<int, 2>{-9, -7};

        for (const int offset : capture_offsets) {
            if ((offset == 7 || offset == -9) && from_file == 0) {
                continue;
            }
            if ((offset == 9 || offset == -7) && from_file == 7) {
                continue;
            }

            const int to = from + offset;
            if (!is_valid_square(to)) {
                continue;
            }

            const Piece target = piece_at(to);
            if (target != Piece::None && piece_color(target) == them && piece_type(target) != PieceType::King) {
                if (from_rank == promotion_rank) {
                    add_promotion_moves(moves, from, to, MoveFlag::PromotionCapture);
                } else {
                    moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::Capture});
                }
            } else if (to == en_passant_square_) {
                moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::EnPassant});
            }
        }
    }

    Bitboard knights = pieces(us, PieceType::Knight);
    while (knights != 0) {
        const int from = pop_lsb(knights);
        for (const auto& [delta_file, delta_rank] : knight_offsets) {
            const int to_file = file_of(from) + delta_file;
            const int to_rank = rank_of(from) + delta_rank;
            if (to_file < 0 || to_file >= 8 || to_rank < 0 || to_rank >= 8) {
                continue;
            }

            const int to = to_rank * 8 + to_file;
            const Piece target = piece_at(to);
            if (target == Piece::None) {
                moves.push_back(Move{.from = from, .to = to});
            } else if (piece_color(target) == them && piece_type(target) != PieceType::King) {
                moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::Capture});
            }
        }
    }

    Bitboard bishops = pieces(us, PieceType::Bishop);
    while (bishops != 0) {
        add_slider_moves(*this, moves, pop_lsb(bishops), us, bishop_directions);
    }

    Bitboard rooks = pieces(us, PieceType::Rook);
    while (rooks != 0) {
        add_slider_moves(*this, moves, pop_lsb(rooks), us, rook_directions);
    }

    Bitboard queens = pieces(us, PieceType::Queen);
    while (queens != 0) {
        const int from = pop_lsb(queens);
        add_slider_moves(*this, moves, from, us, bishop_directions);
        add_slider_moves(*this, moves, from, us, rook_directions);
    }

    Bitboard kings = pieces(us, PieceType::King);
    if (kings != 0) {
        const int from = pop_lsb(kings);
        for (const auto& [delta_file, delta_rank] : king_offsets) {
            const int to_file = file_of(from) + delta_file;
            const int to_rank = rank_of(from) + delta_rank;
            if (to_file < 0 || to_file >= 8 || to_rank < 0 || to_rank >= 8) {
                continue;
            }

            const int to = to_rank * 8 + to_file;
            const Piece target = piece_at(to);
            if (target == Piece::None) {
                moves.push_back(Move{.from = from, .to = to});
            } else if (piece_color(target) == them && piece_type(target) != PieceType::King) {
                moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::Capture});
            }
        }

        if (us == Color::White && from == 4 && !in_check(Color::White)) {
            if ((castling_rights_ & WhiteKingSide) != 0
                && piece_at(5) == Piece::None
                && piece_at(6) == Piece::None
                && piece_at(7) == Piece::WhiteRook
                && !is_square_attacked(5, Color::Black)
                && !is_square_attacked(6, Color::Black)) {
                moves.push_back(Move{.from = 4, .to = 6, .flag = MoveFlag::KingCastle});
            }
            if ((castling_rights_ & WhiteQueenSide) != 0
                && piece_at(3) == Piece::None
                && piece_at(2) == Piece::None
                && piece_at(1) == Piece::None
                && piece_at(0) == Piece::WhiteRook
                && !is_square_attacked(3, Color::Black)
                && !is_square_attacked(2, Color::Black)) {
                moves.push_back(Move{.from = 4, .to = 2, .flag = MoveFlag::QueenCastle});
            }
        } else if (us == Color::Black && from == 60 && !in_check(Color::Black)) {
            if ((castling_rights_ & BlackKingSide) != 0
                && piece_at(61) == Piece::None
                && piece_at(62) == Piece::None
                && piece_at(63) == Piece::BlackRook
                && !is_square_attacked(61, Color::White)
                && !is_square_attacked(62, Color::White)) {
                moves.push_back(Move{.from = 60, .to = 62, .flag = MoveFlag::KingCastle});
            }
            if ((castling_rights_ & BlackQueenSide) != 0
                && piece_at(59) == Piece::None
                && piece_at(58) == Piece::None
                && piece_at(57) == Piece::None
                && piece_at(56) == Piece::BlackRook
                && !is_square_attacked(59, Color::White)
                && !is_square_attacked(58, Color::White)) {
                moves.push_back(Move{.from = 60, .to = 58, .flag = MoveFlag::QueenCastle});
            }
        }
    }

    return moves;
}

std::vector<Move> Board::generate_legal_moves() const
{
    std::vector<Move> legal_moves;
    const auto pseudo_legal_moves = generate_pseudo_legal_moves();
    legal_moves.reserve(pseudo_legal_moves.size());

    for (const Move& move : pseudo_legal_moves) {
        Board next = *this;
        UndoState undo;
        if (!next.make_move(move, undo)) {
            continue;
        }
        if (!next.in_check(side_to_move_)) {
            legal_moves.push_back(move);
        }
    }

    return legal_moves;
}

bool Board::make_move(const Move& move, UndoState& undo)
{
    if (!move.is_valid()) {
        return false;
    }

    undo = UndoState{
        .pieces = pieces_,
        .side_to_move = side_to_move_,
        .castling_rights = castling_rights_,
        .en_passant_square = en_passant_square_,
        .halfmove_clock = halfmove_clock_,
        .fullmove_number = fullmove_number_,
        .hash = hash_,
    };

    const Color us = side_to_move_;
    const Color them = opposite(us);
    const Piece moving = piece_at(move.from);
    if (moving == Piece::None || piece_color(moving) != us) {
        return false;
    }

    const PieceType moving_type = piece_type(moving);
    Piece captured = Piece::None;
    int capture_square = move.to;

    if (move.flag == MoveFlag::EnPassant) {
        if (moving_type != PieceType::Pawn || move.to != en_passant_square_) {
            return false;
        }
        capture_square = us == Color::White ? move.to - 8 : move.to + 8;
        captured = piece_at(capture_square);
        if (captured != make_piece(them, PieceType::Pawn)) {
            return false;
        }
    } else {
        captured = piece_at(move.to);
    }

    if (captured != Piece::None && (piece_color(captured) == us || piece_type(captured) == PieceType::King)) {
        return false;
    }

    clear_square(move.from);
    if (captured != Piece::None) {
        clear_square(capture_square);
    }

    Piece placed = moving;
    if (moving_type == PieceType::Pawn && move.promotion != PieceType::None) {
        placed = make_piece(us, move.promotion);
    }
    put_piece(placed, move.to);

    if (move.flag == MoveFlag::KingCastle) {
        if (us == Color::White) {
            clear_square(7);
            put_piece(Piece::WhiteRook, 5);
        } else {
            clear_square(63);
            put_piece(Piece::BlackRook, 61);
        }
    } else if (move.flag == MoveFlag::QueenCastle) {
        if (us == Color::White) {
            clear_square(0);
            put_piece(Piece::WhiteRook, 3);
        } else {
            clear_square(56);
            put_piece(Piece::BlackRook, 59);
        }
    }

    if (moving_type == PieceType::King) {
        if (us == Color::White) {
            castling_rights_ &= static_cast<std::uint8_t>(~(WhiteKingSide | WhiteQueenSide));
        } else {
            castling_rights_ &= static_cast<std::uint8_t>(~(BlackKingSide | BlackQueenSide));
        }
    }

    if (move.from == 0 || capture_square == 0) {
        castling_rights_ &= static_cast<std::uint8_t>(~WhiteQueenSide);
    }
    if (move.from == 7 || capture_square == 7) {
        castling_rights_ &= static_cast<std::uint8_t>(~WhiteKingSide);
    }
    if (move.from == 56 || capture_square == 56) {
        castling_rights_ &= static_cast<std::uint8_t>(~BlackQueenSide);
    }
    if (move.from == 63 || capture_square == 63) {
        castling_rights_ &= static_cast<std::uint8_t>(~BlackKingSide);
    }

    en_passant_square_ = -1;
    if (moving_type == PieceType::Pawn && move.flag == MoveFlag::DoublePawnPush) {
        en_passant_square_ = us == Color::White ? move.from + 8 : move.from - 8;
    }

    halfmove_clock_ = moving_type == PieceType::Pawn || captured != Piece::None ? 0 : halfmove_clock_ + 1;
    if (us == Color::Black) {
        ++fullmove_number_;
    }

    side_to_move_ = them;
    refresh_hash();
    return true;
}

bool Board::make_null_move(UndoState& undo)
{
    if (in_check(side_to_move_)) {
        return false;
    }

    undo = UndoState{
        .pieces = pieces_,
        .side_to_move = side_to_move_,
        .castling_rights = castling_rights_,
        .en_passant_square = en_passant_square_,
        .halfmove_clock = halfmove_clock_,
        .fullmove_number = fullmove_number_,
        .hash = hash_,
    };

    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }

    side_to_move_ = opposite(side_to_move_);
    en_passant_square_ = -1;
    ++halfmove_clock_;
    refresh_hash();
    return true;
}

void Board::unmake_move(const UndoState& undo)
{
    pieces_ = undo.pieces;
    side_to_move_ = undo.side_to_move;
    castling_rights_ = undo.castling_rights;
    en_passant_square_ = undo.en_passant_square;
    halfmove_clock_ = undo.halfmove_clock;
    fullmove_number_ = undo.fullmove_number;
    hash_ = undo.hash;
}

void Board::clear()
{
    pieces_.fill(0);
    side_to_move_ = Color::White;
    castling_rights_ = 0;
    en_passant_square_ = -1;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    hash_ = 0;
}

void Board::put_piece(Piece piece, int square)
{
    if (piece == Piece::None || !is_valid_square(square)) {
        return;
    }

    pieces_[piece_index(piece)] |= square_bb(square);
}

void Board::remove_piece(Piece piece, int square)
{
    if (piece == Piece::None || !is_valid_square(square)) {
        return;
    }

    pieces_[piece_index(piece)] &= ~square_bb(square);
}

void Board::clear_square(int square)
{
    if (!is_valid_square(square)) {
        return;
    }

    const Bitboard mask = ~square_bb(square);
    for (auto& piece_board : pieces_) {
        piece_board &= mask;
    }
}

void Board::refresh_hash()
{
    hash_ = compute_hash();
}

std::uint64_t Board::compute_hash() const
{
    const auto& keys = zobrist_keys();
    std::uint64_t result = 0;

    for (int index = 0; index < 12; ++index) {
        Bitboard bitboard = pieces_[index];
        while (bitboard != 0) {
            const int square = pop_lsb(bitboard);
            result ^= keys.pieces[index][square];
        }
    }

    result ^= keys.castling[castling_rights_ & 0x0f];
    if (en_passant_square_ != -1) {
        result ^= keys.en_passant_file[file_of(en_passant_square_)];
    }
    if (side_to_move_ == Color::Black) {
        result ^= keys.side_to_move;
    }

    return result;
}

} // namespace aetherz
