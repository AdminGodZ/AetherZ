#include "engine/board.hpp"

#include "engine/attacks.hpp"

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

constexpr Bitboard rank_bb(int rank)
{
    return Bitboard{0xFF} << (8 * rank);
}

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

void add_promotion_moves(MoveList& moves, int from, int to, MoveFlag flag)
{
    for (const PieceType promotion : promotion_pieces) {
        moves.push_back(Move{.from = from, .to = to, .promotion = promotion, .flag = flag});
    }
}

void add_piece_moves(MoveList& moves, int from, Bitboard targets, Bitboard enemies)
{
    while (targets != 0) {
        const int to = pop_lsb(targets);
        const MoveFlag flag = (enemies & square_bb(to)) != 0 ? MoveFlag::Capture : MoveFlag::Quiet;
        moves.push_back(Move{.from = from, .to = to, .flag = flag});
    }
}

// Castling rights that survive a move touching each square.
constexpr std::array<std::uint8_t, 64> castling_masks = [] {
    std::array<std::uint8_t, 64> masks{};
    masks.fill(0x0f);
    masks[0] = 0x0f & ~0x02;
    masks[4] = 0x0f & ~0x03;
    masks[7] = 0x0f & ~0x01;
    masks[56] = 0x0f & ~0x08;
    masks[60] = 0x0f & ~0x0c;
    masks[63] = 0x0f & ~0x04;
    return masks;
}();

struct CastlingRookMove {
    int from = -1;
    int to = -1;
};

CastlingRookMove castling_rook_move(const Move& move, Color us)
{
    if (move.flag == MoveFlag::KingCastle) {
        return us == Color::White ? CastlingRookMove{7, 5} : CastlingRookMove{63, 61};
    }
    if (move.flag == MoveFlag::QueenCastle) {
        return us == Color::White ? CastlingRookMove{0, 3} : CastlingRookMove{56, 59};
    }
    return {};
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

bool Board::can_castle(Color color, bool kingside) const
{
    if (color == Color::White) {
        return (castling_rights_ & (kingside ? WhiteKingSide : WhiteQueenSide)) != 0;
    }

    return (castling_rights_ & (kingside ? BlackKingSide : BlackQueenSide)) != 0;
}

bool Board::is_square_attacked(int square, Color by_color) const
{
    return attackers_to(*this, square, by_color, occupancy()) != 0;
}

bool Board::in_check(Color color) const
{
    const int king = king_square(color);
    return is_valid_square(king) && is_square_attacked(king, opposite(color));
}

Board::CheckInfo Board::check_info() const
{
    const Color us = side_to_move_;
    CheckInfo info;
    info.king = king_square(opposite(us));
    if (!is_valid_square(info.king)) {
        return info;
    }

    const int king = info.king;
    const Bitboard diagonal_rays = bishop_attacks(king, occupancy_);
    const Bitboard straight_rays = rook_attacks(king, occupancy_);
    info.check_squares[piece_type_index(PieceType::Pawn)] = pawn_attacks(opposite(us), king);
    info.check_squares[piece_type_index(PieceType::Knight)] = knight_attacks(king);
    info.check_squares[piece_type_index(PieceType::Bishop)] = diagonal_rays;
    info.check_squares[piece_type_index(PieceType::Rook)] = straight_rays;
    info.check_squares[piece_type_index(PieceType::Queen)] = diagonal_rays | straight_rays;

    // A piece of ours that the king sees along a line is a discoverer if a
    // slider of ours appears behind it once it is gone.
    const Bitboard queens = pieces(us, PieceType::Queen);
    const Bitboard diagonal_sliders = pieces(us, PieceType::Bishop) | queens;
    const Bitboard straight_sliders = pieces(us, PieceType::Rook) | queens;
    const Bitboard own = occupancy(us);
    for (Bitboard blockers = diagonal_rays & own; blockers != 0; blockers &= blockers - 1) {
        const Bitboard blocker = blockers & (0 - blockers);
        if ((bishop_attacks(king, occupancy_ ^ blocker) & diagonal_sliders & ~blocker) != 0) {
            info.discoverers |= blocker;
        }
    }
    for (Bitboard blockers = straight_rays & own; blockers != 0; blockers &= blockers - 1) {
        const Bitboard blocker = blockers & (0 - blockers);
        if ((rook_attacks(king, occupancy_ ^ blocker) & straight_sliders & ~blocker) != 0) {
            info.discoverers |= blocker;
        }
    }
    return info;
}

bool Board::gives_check(const Move& move) const
{
    return gives_check(move, check_info());
}

bool Board::gives_check(const Move& move, const CheckInfo& info) const
{
    const Color us = side_to_move_;
    const int king = info.king;
    if (!is_valid_square(king)) {
        return false;
    }

    // Rare special moves: just play them.
    if (move.flag == MoveFlag::EnPassant
        || move.flag == MoveFlag::KingCastle
        || move.flag == MoveFlag::QueenCastle
        || move.promotion != PieceType::None) {
        Board next = *this;
        UndoState undo;
        return next.make_move(move, undo) && next.in_check(opposite(us));
    }

    // The side not to move is never in check, so a piece attacks the king
    // from its target square exactly when that square is a check square:
    // the square it leaves can only open a line it was not checking along.
    const PieceType type = piece_type(squares_[move.from]);
    if ((info.check_squares[piece_type_index(type)] & square_bb(move.to)) != 0) {
        return true;
    }

    // Discovered check, unless the piece stays on the line it blocked.
    if ((info.discoverers & square_bb(move.from)) == 0) {
        return false;
    }
    const Bitboard occupancy = (occupancy_ & ~square_bb(move.from)) | square_bb(move.to);
    const Bitboard queens = pieces(us, PieceType::Queen);
    const Bitboard diagonal = (pieces(us, PieceType::Bishop) | queens) & ~square_bb(move.from);
    const Bitboard straight = (pieces(us, PieceType::Rook) | queens) & ~square_bb(move.from);
    return (bishop_attacks(king, occupancy) & diagonal) != 0 || (rook_attacks(king, occupancy) & straight) != 0;
}

template <bool TacticalOnly>
void Board::generate_moves(MoveList& moves) const
{
    const Color us = side_to_move_;
    const Color them = opposite(us);
    const Bitboard own = occupancy(us);
    // Kings are never captured: positions with a capturable king are illegal.
    const Bitboard enemies = occupancy(them) & ~pieces(them, PieceType::King);
    const Bitboard empty = ~occupancy_;
    const Bitboard targets = TacticalOnly ? enemies : (empty | enemies);
    const int forward = us == Color::White ? 8 : -8;
    const Bitboard promotion_rank = us == Color::White ? rank_bb(6) : rank_bb(1);
    const Bitboard start_rank = us == Color::White ? rank_bb(1) : rank_bb(6);

    Bitboard pawns = pieces(us, PieceType::Pawn);
    while (pawns != 0) {
        const int from = pop_lsb(pawns);
        const bool promotes = (promotion_rank & square_bb(from)) != 0;
        const int one_forward = from + forward;

        if ((empty & square_bb(one_forward)) != 0) {
            if (promotes) {
                add_promotion_moves(moves, from, one_forward, MoveFlag::Promotion);
            } else if (!TacticalOnly) {
                moves.push_back(Move{.from = from, .to = one_forward});
                const int two_forward = one_forward + forward;
                if ((start_rank & square_bb(from)) != 0 && (empty & square_bb(two_forward)) != 0) {
                    moves.push_back(Move{.from = from, .to = two_forward, .flag = MoveFlag::DoublePawnPush});
                }
            }
        }

        Bitboard captures = pawn_attacks(us, from) & enemies;
        while (captures != 0) {
            const int to = pop_lsb(captures);
            if (promotes) {
                add_promotion_moves(moves, from, to, MoveFlag::PromotionCapture);
            } else {
                moves.push_back(Move{.from = from, .to = to, .flag = MoveFlag::Capture});
            }
        }

        if (en_passant_square_ != -1 && (pawn_attacks(us, from) & square_bb(en_passant_square_)) != 0) {
            moves.push_back(Move{.from = from, .to = en_passant_square_, .flag = MoveFlag::EnPassant});
        }
    }

    Bitboard knights = pieces(us, PieceType::Knight);
    while (knights != 0) {
        const int from = pop_lsb(knights);
        add_piece_moves(moves, from, knight_attacks(from) & targets, enemies);
    }

    Bitboard bishops = pieces(us, PieceType::Bishop) | pieces(us, PieceType::Queen);
    while (bishops != 0) {
        const int from = pop_lsb(bishops);
        add_piece_moves(moves, from, bishop_attacks(from, occupancy_) & targets, enemies);
    }

    Bitboard rooks = pieces(us, PieceType::Rook) | pieces(us, PieceType::Queen);
    while (rooks != 0) {
        const int from = pop_lsb(rooks);
        add_piece_moves(moves, from, rook_attacks(from, occupancy_) & targets, enemies);
    }

    const int king = king_square(us);
    if (!is_valid_square(king)) {
        return;
    }
    add_piece_moves(moves, king, king_attacks(king) & targets & ~own, enemies);

    if constexpr (!TacticalOnly) {
        const int home = us == Color::White ? 4 : 60;
        if (king != home || (castling_rights_ & (us == Color::White ? 0x03 : 0x0c)) == 0 || in_check(us)) {
            return;
        }

        const Piece rook = make_piece(us, PieceType::Rook);
        if (can_castle(us, true)
            && piece_at(home + 1) == Piece::None
            && piece_at(home + 2) == Piece::None
            && piece_at(home + 3) == rook
            && !is_square_attacked(home + 1, them)
            && !is_square_attacked(home + 2, them)) {
            moves.push_back(Move{.from = home, .to = home + 2, .flag = MoveFlag::KingCastle});
        }
        if (can_castle(us, false)
            && piece_at(home - 1) == Piece::None
            && piece_at(home - 2) == Piece::None
            && piece_at(home - 3) == Piece::None
            && piece_at(home - 4) == rook
            && !is_square_attacked(home - 1, them)
            && !is_square_attacked(home - 2, them)) {
            moves.push_back(Move{.from = home, .to = home - 2, .flag = MoveFlag::QueenCastle});
        }
    }
}

MoveList Board::generate_pseudo_legal_moves() const
{
    MoveList moves;
    generate_moves<false>(moves);
    return moves;
}

MoveList Board::generate_pseudo_legal_tactical_moves() const
{
    MoveList moves;
    generate_moves<true>(moves);
    return moves;
}

MoveList Board::generate_legal_moves() const
{
    MoveList legal_moves;
    Board next = *this;
    for (const Move& move : generate_pseudo_legal_moves()) {
        UndoState undo;
        if (!next.make_move(move, undo)) {
            continue;
        }
        if (!next.in_check(side_to_move_)) {
            legal_moves.push_back(move);
        }
        next.unmake_move(undo);
    }

    return legal_moves;
}

bool Board::make_move(const Move& move, UndoState& undo)
{
    if (!move.is_valid()) {
        return false;
    }

    const Color us = side_to_move_;
    const Color them = opposite(us);
    const Piece moving = squares_[move.from];
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
        captured = squares_[capture_square];
        if (captured != make_piece(them, PieceType::Pawn)) {
            return false;
        }
    } else {
        captured = squares_[move.to];
    }

    if (captured != Piece::None && (piece_color(captured) == us || piece_type(captured) == PieceType::King)) {
        return false;
    }

    undo = UndoState{
        .move = move,
        .moved = moving,
        .captured = captured,
        .captured_square = capture_square,
        .null_move = false,
        .castling_rights = castling_rights_,
        .en_passant_square = en_passant_square_,
        .halfmove_clock = halfmove_clock_,
        .fullmove_number = fullmove_number_,
        .hash = hash_,
    };

    const auto& keys = zobrist_keys();
    std::uint64_t hash = hash_;

    if (captured != Piece::None) {
        clear_square(capture_square);
        hash ^= keys.pieces[piece_index(captured)][capture_square];
    }

    const Piece placed = moving_type == PieceType::Pawn && move.promotion != PieceType::None
        ? make_piece(us, move.promotion)
        : moving;
    clear_square(move.from);
    put_piece(placed, move.to);
    hash ^= keys.pieces[piece_index(moving)][move.from] ^ keys.pieces[piece_index(placed)][move.to];

    if (const CastlingRookMove rook_move = castling_rook_move(move, us); rook_move.from != -1) {
        const Piece rook = make_piece(us, PieceType::Rook);
        move_piece(rook, rook_move.from, rook_move.to);
        hash ^= keys.pieces[piece_index(rook)][rook_move.from] ^ keys.pieces[piece_index(rook)][rook_move.to];
    }

    hash ^= keys.castling[castling_rights_];
    castling_rights_ &= castling_masks[move.from] & castling_masks[capture_square];
    hash ^= keys.castling[castling_rights_];

    if (en_passant_square_ != -1) {
        hash ^= keys.en_passant_file[file_of(en_passant_square_)];
    }
    en_passant_square_ = -1;
    if (moving_type == PieceType::Pawn && move.flag == MoveFlag::DoublePawnPush) {
        en_passant_square_ = us == Color::White ? move.from + 8 : move.from - 8;
        hash ^= keys.en_passant_file[file_of(en_passant_square_)];
    }

    halfmove_clock_ = moving_type == PieceType::Pawn || captured != Piece::None ? 0 : halfmove_clock_ + 1;
    if (us == Color::Black) {
        ++fullmove_number_;
    }

    side_to_move_ = them;
    hash_ = hash ^ keys.side_to_move;
    return true;
}

bool Board::make_null_move(UndoState& undo)
{
    if (in_check(side_to_move_)) {
        return false;
    }

    undo = UndoState{
        .move = {},
        .moved = Piece::None,
        .captured = Piece::None,
        .captured_square = -1,
        .null_move = true,
        .castling_rights = castling_rights_,
        .en_passant_square = en_passant_square_,
        .halfmove_clock = halfmove_clock_,
        .fullmove_number = fullmove_number_,
        .hash = hash_,
    };

    const auto& keys = zobrist_keys();
    if (en_passant_square_ != -1) {
        hash_ ^= keys.en_passant_file[file_of(en_passant_square_)];
    }
    if (side_to_move_ == Color::Black) {
        ++fullmove_number_;
    }

    side_to_move_ = opposite(side_to_move_);
    en_passant_square_ = -1;
    ++halfmove_clock_;
    hash_ ^= keys.side_to_move;
    return true;
}

void Board::unmake_move(const UndoState& undo)
{
    side_to_move_ = opposite(side_to_move_);
    castling_rights_ = undo.castling_rights;
    en_passant_square_ = undo.en_passant_square;
    halfmove_clock_ = undo.halfmove_clock;
    fullmove_number_ = undo.fullmove_number;
    hash_ = undo.hash;

    if (undo.null_move) {
        return;
    }

    const Move& move = undo.move;
    if (const CastlingRookMove rook_move = castling_rook_move(move, side_to_move_); rook_move.from != -1) {
        move_piece(make_piece(side_to_move_, PieceType::Rook), rook_move.to, rook_move.from);
    }

    clear_square(move.to);
    put_piece(undo.moved, move.from);
    if (undo.captured != Piece::None) {
        put_piece(undo.captured, undo.captured_square);
    }
}

void Board::clear()
{
    pieces_.fill(0);
    squares_.fill(Piece::None);
    occupancies_.fill(0);
    occupancy_ = 0;
    king_squares_.fill(-1);
    side_to_move_ = Color::White;
    castling_rights_ = 0;
    en_passant_square_ = -1;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    hash_ = 0;
}

// Places a piece on an empty square. Does not update the hash.
void Board::put_piece(Piece piece, int square)
{
    const Bitboard mask = square_bb(square);
    pieces_[piece_index(piece)] |= mask;
    squares_[square] = piece;
    occupancies_[color_index(piece_color(piece))] |= mask;
    occupancy_ |= mask;

    if (piece_type(piece) == PieceType::King) {
        king_squares_[color_index(piece_color(piece))] = square;
    }
}

// Removes whatever stands on a square. Does not update the hash.
void Board::clear_square(int square)
{
    const Piece piece = squares_[square];
    if (piece == Piece::None) {
        return;
    }

    const Bitboard mask = square_bb(square);
    pieces_[piece_index(piece)] &= ~mask;
    occupancies_[color_index(piece_color(piece))] &= ~mask;
    occupancy_ &= ~mask;
    squares_[square] = Piece::None;
}

void Board::move_piece(Piece piece, int from, int to)
{
    const Bitboard mask = square_bb(from) | square_bb(to);
    pieces_[piece_index(piece)] ^= mask;
    occupancies_[color_index(piece_color(piece))] ^= mask;
    occupancy_ ^= mask;
    squares_[from] = Piece::None;
    squares_[to] = piece;
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
