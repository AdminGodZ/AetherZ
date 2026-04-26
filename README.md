# AetherZ

AetherZ is a high-performance chess engine for competitive play and deep position analysis.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

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
./build/aetherz searchbench 2
./build/aetherz tactics 3
./build/aetherz selfplay 1 2 80
./build/aetherz epdtest testsuites/arasan2024.epd 3
./build/aetherz arasan 3
```

## UCI Examples

```text
setoption name Hash value 128
go depth 5
go movetime 1000
go wtime 60000 btime 60000 winc 1000 binc 1000
```
