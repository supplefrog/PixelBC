#!/usr/bin/env python3
"""Write one bounded, representative mutation of the actual production shader.

The independent GPU/detex checks must reject these successfully compiled modules.
This never edits the original shader or any oracle/expected pixels.
"""
import argparse
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mutation", choices=("zero-rgb", "zero-alpha"))
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    text = (ROOT / "src/bc_etc2_encode.comp").read_text()
    old, new = {
        "zero-rgb": (
            "dst.words[target]=rgb.x; dst.words[target+1u]=rgb.y;",
            "dst.words[target]=0u; dst.words[target+1u]=0u;",
        ),
        "zero-alpha": (
            "dst.words[target]=alpha.x; dst.words[target+1u]=alpha.y; target+=2u;",
            "dst.words[target]=0u; dst.words[target+1u]=0u; target+=2u;",
        ),
    }[args.mutation]
    if text.count(old) != 1:
        raise RuntimeError("Production store anchor is ambiguous or missing")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as output:
        output.write(text.replace(old, new))


if __name__ == "__main__":
    main()
