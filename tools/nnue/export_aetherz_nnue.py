#!/usr/bin/env python3
"""Export simple tensor JSON into AetherZ's native text .nnue format."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable


MAGIC = "AETHERZ_NNUE_V1"


def row(values: Iterable[int]) -> str:
    return " ".join(str(int(value)) for value in values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="JSON tensor file")
    parser.add_argument("output", type=Path, help="AetherZ .nnue output")
    args = parser.parse_args()

    data = json.loads(args.input.read_text(encoding="utf-8"))
    metadata = data.get("metadata", {})
    accumulator_size = int(metadata.get("accumulator_size", 768))
    hidden_size = int(metadata.get("hidden_size", 32))

    lines: list[str] = [
        MAGIC,
        f"name={metadata.get('name', 'exported')}",
        f"feature_set={metadata.get('feature_set', 'halfka-v1')}",
        f"accumulator_size={accumulator_size}",
        f"hidden_size={hidden_size}",
        f"input_scale={int(metadata.get('input_scale', 1))}",
        f"hidden_scale={int(metadata.get('hidden_scale', 1))}",
        f"output_scale={int(metadata.get('output_scale', 1))}",
    ]

    accumulator_bias = data.get("accumulator_bias")
    if accumulator_bias is not None:
        lines.append(f"accumulator_bias {row(accumulator_bias)}")

    for feature, weights in sorted(data.get("features", {}).items(), key=lambda item: int(item[0])):
        lines.append(f"feature {int(feature)} {row(weights)}")

    hidden1_bias = data.get("hidden1_bias")
    if hidden1_bias is not None:
        lines.append(f"hidden1_bias {row(hidden1_bias)}")

    for index, weights in sorted(data.get("hidden1_weights", {}).items(), key=lambda item: int(item[0])):
        lines.append(f"hidden1_weight {int(index)} {row(weights)}")

    hidden2_bias = data.get("hidden2_bias")
    if hidden2_bias is not None:
        lines.append(f"hidden2_bias {row(hidden2_bias)}")

    for index, weights in sorted(data.get("hidden2_weights", {}).items(), key=lambda item: int(item[0])):
        lines.append(f"hidden2_weight {int(index)} {row(weights)}")

    lines.append(f"output_bias {int(data.get('output_bias', 0))}")
    lines.append(f"output_weight {row(data.get('output_weights', [0] * hidden_size))}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
