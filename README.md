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

## UCI Examples

```text
setoption name Hash value 128
go depth 5
go movetime 1000
go wtime 60000 btime 60000 winc 1000 binc 1000
```
