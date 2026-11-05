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

Use Bullet as the trainer, then export the learned tensors to JSON and convert them:

```sh
python tools/nnue/export_aetherz_nnue.py model.json nets/aetherz.nnue
```

Bullet training needs a CUDA or ROCm runtime enabled in the Bullet build, for
example `cargo run --release --features cuda --package bullet_lib --example ...`.
The native AetherZ loader does not consume Bullet checkpoint files directly yet;
export the tensors into the JSON shape expected by `export_aetherz_nnue.py`.

For engine smoke tests without training:

```sh
python tools/nnue/make_tiny_nnue.py /tmp/tiny.nnue --mode queen-a1
./build/aetherz
```

The native format is intentionally AetherZ-specific for now. Stockfish `.nnue`
compatibility can come later after the native HalfKA path is stable.
