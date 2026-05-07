#!/usr/bin/env python3
"""Create deterministic tiny AetherZ NNUE files for tests and smoke checks."""

from __future__ import annotations

import argparse
from pathlib import Path


MAGIC = "AETHERZ_NNUE_V1"


def piece_index(piece: str) -> int:
    pieces = "PNBRQKpnbrqk"
    return pieces.index(piece)


def square_index(square: str) -> int:
    file = ord(square[0]) - ord("a")
    rank = int(square[1]) - 1
    return rank * 8 + file


def orient_square(color: str, square: int) -> int:
    return square if color == "white" else square ^ 56


def halfka_feature(color: str, king_square: str, piece: str, piece_square: str) -> int:
    king = orient_square(color, square_index(king_square))
    target = orient_square(color, square_index(piece_square))
    return (king * 12 + piece_index(piece)) * 64 + target


def constant_net(output: Path, value: int) -> None:
    output.write_text(
        "\n".join(
            [
                MAGIC,
                "name=tiny-constant",
                "feature_set=halfka-v1",
                "accumulator_size=2",
                "hidden_size=1",
                "input_scale=1",
                "hidden_scale=1",
                "output_scale=1",
                f"output_bias {value}",
                "output_weight 0",
                "",
            ]
        ),
        encoding="utf-8",
    )


def queen_a1_net(output: Path) -> None:
    feature = halfka_feature("white", "e1", "Q", "a1")
    output.write_text(
        "\n".join(
            [
                MAGIC,
                "name=tiny-queen-a1",
                "feature_set=halfka-v1",
                "accumulator_size=2",
                "hidden_size=1",
                "input_scale=1",
                "hidden_scale=1",
                "output_scale=1",
                "accumulator_bias 0 0",
                f"feature {feature} 32 0",
                "hidden1_bias 0",
                "hidden1_weight 0 1",
                "hidden2_bias 0",
                "hidden2_weight 0 1",
                "output_bias 0",
                "output_weight 1",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--mode", choices=["constant", "queen-a1"], default="constant")
    parser.add_argument("--value", type=int, default=25)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.mode == "queen-a1":
        queen_a1_net(args.output)
    else:
        constant_net(args.output, args.value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
