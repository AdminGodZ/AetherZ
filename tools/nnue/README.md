# AetherZ NNUE Tooling

Generate first training positions:

```sh
./build/aetherz traindata data/aetherz-train.txt 10000 3 80
```

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
