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
```
