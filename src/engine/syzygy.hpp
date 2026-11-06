#pragma once

#include "engine/board.hpp"

#include <optional>
#include <string>
#include <vector>

// Syzygy endgame tablebases, probed with Fathom (third_party/fathom). The
// tables are global: load them only while no search runs.
namespace aetherz::syzygy {

// Loads the tables found in path, a list of folders separated by ':' (';'
// on Windows). An empty path or "<empty>" unloads them. Returns false if
// the path could not be used.
bool init(const std::string& path);

// The most pieces (kings included) of any loaded table; 0 without tables.
[[nodiscard]] int max_pieces();

// Win/draw/loss for the side to move. Blessed losses and cursed wins are
// losses and wins that the fifty-move rule turns into draws.
enum class Wdl {
    Loss = -2,
    BlessedLoss = -1,
    Draw = 0,
    CursedWin = 1,
    Win = 2,
};

// Probes the WDL tables. Fails (nullopt) unless the position is in the
// tables, the halfmove clock is 0 and no castling rights are left.
// Thread-safe.
[[nodiscard]] std::optional<Wdl> probe_wdl(const Board& board);

struct RootMove {
    Move move;
    // Higher is better; moves with the same rank keep the same result.
    int rank = 0;
    // The result in centipawns: a win is mate_score - max_search_ply - 1.
    int score = 0;
};

struct RootProbe {
    // Every legal move, best rank first.
    std::vector<RootMove> moves;
    // Ranked by distance to zeroing (DTZ); false when only WDL tables were
    // available, so that the ranks only tell win, draw and loss apart.
    bool dtz = false;
};

// Ranks the legal moves of a root position in the tables, as Stockfish does:
// with the DTZ tables, or with the WDL tables when DTZ tables are missing.
// has_repeated says whether a position has repeated since the last capture
// or pawn move. Fails unless every move could be probed. Not thread-safe.
[[nodiscard]] std::optional<RootProbe> probe_root(const Board& board, bool has_repeated, bool use_rule50);

} // namespace aetherz::syzygy
