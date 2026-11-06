# AetherZ NNUE Tooling

Generate training positions with self-play:

```sh
./build/aetherz datagen data/aetherz-train.txt --games 100000 --threads 22 --nodes 5000
```

- Each game starts with 8 or 9 random moves (`--random-plies`) and is then
  played by the engine at a fixed node budget per move (`--nodes`), so the
  data does not depend on machine speed or load.
- Games are played to a real result: mate, stalemate, repetition, fifty
  moves, insufficient material, or adjudication once one side leads by 2000
  cp for six plies (or the score stays within 10 cp for twelve plies after
  ply 80). Openings scored beyond 1000 cp are replayed.
- A position is kept when the side to move is not in check, the move played
  from it is not a capture or promotion, and its score is not a mate score.
- `--eval-file <quantised.bin>` plays the games with a network instead of
  the classical evaluation (`<embedded>` selects a built-in network), for
  training the next network on the previous one's games.
- Every game depends only on `--seed` and its game number, so the output is
  the same whatever the thread count. Use a new seed for every new batch.
  Output is appended to the file.

The output format is Bullet's simple text shape:

```text
<FEN> | <white-relative centipawn score> | <white-relative result>
```

The result is written as `1.0`, `0.5`, or `0.0`, matching Bullet's text
converter. Convert it to BulletFormat with Bullet's utilities:

```sh
bullet-utils convert --from text --input data/aetherz-train.txt --output data/aetherz-train.bf
```

## Training

`tools/nnue/trainer/` is a small Rust crate: [Bullet](https://github.com/jw1912/bullet)'s
`examples/simple.rs` with `HIDDEN_SIZE = 256` and command-line options. It
depends on Bullet at a pinned commit. The engine's network is fixed at
compile time (`src/engine/nnue/network.hpp`) and the trainer matches it:

- inputs `Chess768`, `(768 -> 256)x2 -> 1`, SCReLU, side to move first
- `QA = 255`, `QB = 64`, `SCALE = 400`
- save format `l0w`, `l0b` quantised by QA, `l1w` by QB, `l1b` by QA * QB

Bullet needs a GPU backend. The crate enables `cuda` by default; use
`--no-default-features --features rocm` (or `metal` on a Mac) otherwise.
CUDA builds need `CUDA_PATH` pointing at the toolkit (with `lib64/`).

```sh
cd tools/nnue/trainer
cargo run --release -- --help
cargo run --release -- data/train.bf --output-dir checkpoints --net-id aetherz \
    --superbatches 40 --start-lr 0.001 --final-lr 0.0000081 --wdl 0.75
```

Without arguments other than the data the defaults are those of `simple`:
40 superbatches of 6104 batches of 16384 positions, AdamW, LR 0.001 dropping
10x every 18 superbatches, WDL 0.75, a checkpoint every 10 superbatches and
at the end. `--final-lr` switches to cosine decay to that LR.

## Full pipeline (WSL2 + RTX 5080)

The training PC runs everything inside WSL2 Ubuntu 24.04, user-local:
CUDA 12.9.1 in `~/cuda-12.9.1`, Rust via rustup, Bullet in `~/bullet`.
`~/.nnue_env.sh` (sourced from `~/.profile` and `~/.bashrc`) sets
`CUDA_PATH`, `CUDA_HOME`, `PATH` and `LD_LIBRARY_PATH` (including
`/usr/lib/wsl/lib`, where the Windows driver puts `libcuda`).

```sh
# 0. Tools (once)
cmake -S . -B build-datagen -G Ninja -DCMAKE_BUILD_TYPE=Release -DAETHERZ_NATIVE=ON
cmake --build build-datagen
(cd ~/bullet && cargo build -r --package bullet-utils)
(cd tools/nnue/trainer && cargo build -r)
U=~/bullet/target/release/bullet-utils

# 1. Self-play data, a new seed for every batch. Survives the shell closing.
setsid nohup ./build-datagen/aetherz datagen ~/nnue-data/batch02.txt \
    --threads 20 --seed 2 --games 400000 > ~/nnue-data/batch02.log 2>&1 < /dev/null &

# 2. Text -> BulletFormat (32 bytes a position), then shuffle each batch.
#    Datagen writes whole games in order, so never train on unshuffled data.
$U convert --from text --input ~/nnue-data/batch02.txt --output ~/nnue-data/batch02.bf --threads 8
$U shuffle --input ~/nnue-data/batch02.bf --output ~/nnue-data/batch02-shuf.bf --mem-used-mb 8192

# 3. Merge shuffled batches into one training file.
$U interleave ~/nnue-data/batch01-shuf.bf ~/nnue-data/batch02-shuf.bf --output ~/nnue-data/train.bf

# 4. Train (or pass several .bf files directly).
tools/nnue/trainer/target/release/aetherz-trainer ~/nnue-data/train.bf \
    --output-dir ~/nnue-checkpoints --net-id aetherz-v1 \
    --superbatches 40 --batches-per-superbatch 610 --final-lr 0.0000081

# 5. Load in the engine (UCI).
#    setoption name EvalFile value /home/<user>/nnue-checkpoints/aetherz-v1-40/quantised.bin
```

Rough numbers on this machine: datagen with 20 threads makes about 3000
games (about 275k positions) a minute, 91 positions a game; training runs at
20-26M positions a second, so a 40 x 100M-position run takes about 3 minutes
of GPU time. Data is the bottleneck: size `--batches-per-superbatch` so a run
sees each position a handful of times (`610` x 16384 x 40 is about 400M
positions, about 10 passes over 40M positions), and add data before adding
superbatches.

## Networks

| File | Data | Training | Result |
|---|---|---|---|
| `networks/aetherz-v1-w30.bin` | 39.1M positions, 430k classical self-play games at 5000 nodes (seed 1) | 40 superbatches x 610 batches x 16384, WDL 0.3, cosine LR 0.001 -> 8.1e-6 | +330 Elo [255, 443] vs classical, 100 games at 8+0.08 (WDL 0.75: +118) |

## Using a network

Each checkpoint contains `quantised.bin`: the raw little-endian int16
weights with no header, padded to a multiple of 64 bytes. Load it as is:

```text
setoption name EvalFile value checkpoints/simple-40/quantised.bin
```

The engine is built with a network inside the executable: by default
`networks/aetherz-v1-w30.bin`, or the one given with
`-DAETHERZ_EVALFILE=path/to/quantised.bin` (empty for none). It loads it at
start-up (`EvalFile` defaults to `<embedded>`, and `bench` uses it, so the
bench node count differs from builds without a network); setting `EvalFile`
to a file still replaces it, and `setoption name EvalFile value <embedded>`
switches back.

The engine answers `info string loaded NNUE <path>`, or
`info string failed to load NNUE: <reason>` if the file does not have the
size of this architecture. `UseNNUE false` switches back to the classical
evaluation, which is also used when no network is loaded.
