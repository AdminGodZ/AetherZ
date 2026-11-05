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

Train with [Bullet](https://github.com/jw1912/bullet)'s `examples/simple.rs`,
changing only `HIDDEN_SIZE` to 256 (and the data loader to your data). The
engine's network is fixed at compile time (`src/engine/nnue/network.hpp`)
and must match the example's other settings:

- inputs `Chess768`, `(768 -> 256)x2 -> 1`, SCReLU, side to move first
- `QA = 255`, `QB = 64`, `SCALE = 400`
- save format `l0w`, `l0b` quantised by QA, `l1w` by QB, `l1b` by QA * QB

Bullet needs a GPU backend, for example
`cargo run --release --features cuda --package bullet_lib --example simple`
(or `rocm`, or `metal` on a Mac).

## Using a network

Each checkpoint contains `quantised.bin`: the raw little-endian int16
weights with no header, padded to a multiple of 64 bytes. Load it as is:

```text
setoption name EvalFile value checkpoints/simple-40/quantised.bin
```

The engine answers `info string loaded NNUE <path>`, or
`info string failed to load NNUE: <reason>` if the file does not have the
size of this architecture. `UseNNUE false` switches back to the classical
evaluation, which is also used when no network is loaded.
