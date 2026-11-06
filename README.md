# AetherZ

AetherZ is a high-performance chess engine for competitive play and deep position analysis.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The default build type is `Release` with `-O3`, link-time optimization and
`-march=native` (`-mcpu=native` on ARM). Use `-DAETHERZ_NATIVE=OFF` for a
portable binary and `-DCMAKE_BUILD_TYPE=Debug` when debugging. Always measure
and play with a Release build: a Debug build is many times slower.

The engine evaluates positions with an NNUE network built into the
executable, `networks/aetherz-v2c2-w30.bin` by default (see
[tools/nnue](tools/nnue/README.md) for how networks are trained). Configure
with `-DAETHERZ_EVALFILE=` for an engine that uses the classical evaluation,
or point it at another network file.

## Run

```bash
./build/aetherz
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

The Syzygy tests need some tables (at least KQvK, KRvK and KPvK, WDL and
DTZ) and are skipped unless `AETHERZ_SYZYGY_PATH` names their folder.

## Tools

```bash
./build/aetherz perft 3
./build/aetherz divide 2
./build/aetherz bench
./build/aetherz perftbench
./build/aetherz datagen data/train.txt --games 1000 --threads 8
./build/aetherz searchbench 2
./build/aetherz tactics 3
./build/aetherz selfplay 1 2 80
./build/aetherz epdtest testsuites/arasan2024.epd 3
./build/aetherz arasan 3
```

## Bench

`aetherz bench [depth]` searches a fixed set of positions to a fixed depth
(default 10) and prints the total node count and speed. The node count is a
signature of the search: a change that is only meant to make the engine faster
must keep it identical, while any change to search or evaluation behaviour
changes it. Mention the bench node count in every commit that touches search.
The count depends on the built-in network; with no network (`-DAETHERZ_EVALFILE=`)
it is the classical evaluation's.

## Testing Changes With Matches

Strength changes are only kept if they win games. `aetherz_match` plays two
UCI engines against each other in game pairs (same opening, colors swapped)
and runs a sequential probability ratio test (SPRT) on the results:

```bash
# Build the baseline from master and the candidate from your branch first.
./build/aetherz_match \
    --engine1 ./aetherz-dev --name1 dev \
    --engine2 ./aetherz-base --name2 base \
    --tc 8+0.08 --concurrency 8 --pairs 20000 \
    --sprt 0 5 --games games.txt
```

- The runner uses POSIX processes and pipes, so it builds on Linux, macOS
  and WSL but not natively on Windows; there, fastchess or cutechess work
  just as well.
- `--sprt 0 5` tests "dev gains at least 5 Elo" against "dev gains nothing"
  (alpha = beta = 0.05). Use bounds like `--sprt -5 0` to show that a
  simplification or cleanup does not lose strength.
- Without `--openings`, balanced random openings are generated from a fixed
  seed, so both engines and repeated runs see the same openings. Pass an EPD
  or FEN file with `--openings book.epd` to use a book instead.
- Games are adjudicated as wins after both engines agree on a score of at
  least 1000 cp for six plies, and as draws after twelve plies within 10 cp
  past ply 80.
- `--games` appends every finished game (result, reason, opening, moves).
- Generating thousands of random openings takes a few minutes; save them
  once with `--save-openings book.epd` and pass `--openings book.epd` later.
- The SPRT never concludes before 50 game pairs: variance estimates from a
  handful of pairs are unreliable.

## Tuning The Evaluation

The evaluation weights live in `src/engine/eval_weights.inc` and are fitted
to game results with `aetherz_tune` (Texel tuning). Generate games with the
match runner, then tune and rebuild:

```bash
./build/aetherz_match --engine1 ./build/aetherz --engine2 ./build/aetherz \
    --tc 1+0.01 --concurrency 12 --pairs 20000 --games selfplay.txt
./build/aetherz_tune --out src/engine/eval_weights.inc --epochs 2000 \
    --regularization 0.0000003 selfplay.txt
cmake --build build
```

The tuner keeps quiet positions (not in check, and the move played was not a
capture or promotion), skips the opening plies, fits the logistic scale once
and then runs Adam on all weights. The pawn value is held fixed, and the L2
regularization pulls weights towards their starting values: without it, a
few thousand games overfit badly (an unregularized run lost about 140 Elo,
while the regularized one won about 200). Always confirm a new set of
weights with an SPRT before committing it.

## UCI Examples

```text
setoption name Hash value 128
go depth 5
go movetime 1000
go wtime 60000 btime 60000 winc 1000 binc 1000
```

## Syzygy Tablebases

AetherZ uses Syzygy endgame tablebases the way Stockfish does, through
[Fathom](third_party/fathom) (MIT). The tables are not part of the engine:
download them separately, for example the 3-, 4- and 5-piece set (about
1 GB) from https://tablebase.lichess.ovh/tables/standard/, and point the
engine at the folder (several folders are separated by `:`, or `;` on
Windows):

```text
setoption name SyzygyPath value /path/to/syzygy
```

- At the root, a position in the tables searches only the moves that keep
  its best result, ranked by the DTZ tables (or the WDL tables when DTZ
  tables are missing), and reports the tablebase score unless it finds a
  mate.
- In the search, positions in the tables right after a capture or pawn move
  (and without castling rights) take their result from the WDL tables.
- `SyzygyProbeLimit` (default 7) caps the number of pieces probed,
  `SyzygyProbeDepth` (default 1) is the least depth at which positions with
  exactly that many pieces are probed, and `Syzygy50MoveRule` (default true)
  scores wins and losses that the fifty-move rule turns into draws as
  draws. `info` lines report `tbhits`.

