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

## Run

```bash
./build/aetherz
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Tools

```bash
./build/aetherz perft 3
./build/aetherz divide 2
./build/aetherz bench
./build/aetherz perftbench
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

## UCI Examples

```text
setoption name Hash value 128
go depth 5
go movetime 1000
go wtime 60000 btime 60000 winc 1000 binc 1000
```
