#!/usr/bin/env python3
"""Independent exhaustive EAC palette bound for a synthetic BC3 fixture.

Uses the pinned, unchanged detex/spec modifier table. No candidate fitting
logic or sampled readback is used to construct the expectation.
"""
import collections
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = bytes.fromhex('139a74bd084cebe81d9b2bac99c34236')


def main():
    a, z = SOURCE[:2]
    bits = int.from_bytes(SOURCE[2:8], 'little')
    indices = [(bits >> (p * 3)) & 7 for p in range(16)]
    assert a <= z
    values = [a, z] + [((5 - n) * a + n * z) // 5 for n in range(1, 5)] + [0, 255]
    used = collections.Counter(values[i] for i in indices)
    text = (ROOT / 'tests/oracle/detex/decompress-eac.c').read_text()
    start = text.index('eac_modifier_table[16][8]')
    table = text[start:text.index('};', start)]
    tables = [list(map(int, re.findall(r'-?\d+', row)))
              for row in re.findall(r'\{([^{}]+)\}', table)]
    assert len(tables) == 16 and all(len(t) == 8 for t in tables)
    best = None
    count = 0
    for t, mod in enumerate(tables):
        for mult in range(1, 16):
            for base in range(256):
                palette = [max(0, min(255, base + mult * m)) for m in mod]
                if (0 in used and 0 not in palette) or (255 in used and 255 not in palette):
                    continue
                count += 1
                error = {v: min(abs(v - p) for p in palette) for v in used}
                key = (max(error.values()), sum(used[v] * e * e for v, e in error.items()))
                if best is None or key < best['key']:
                    best = dict(key=key, table=t, multiplier=mult, base=base,
                                palette=palette, error=error)
    result = dict(source_BC=SOURCE.hex(), endpoints=[a, z], selector_indices=indices,
                  independent_truncated_alpha_palette=values, used_values_counts=dict(used),
                  enumerated_all_encode_legal_palettes=16 * 15 * 256,
                  exact_endpoint_legal_palettes=count,
                  objective='minimum peak byte error, then weighted squared error; exact original0/255',
                  minimum_peak_error=best['key'][0], optimal=best,
                  oracle_modifier_source='unmodified pinned detex; EAC integer palette clamp specification',
                  pass_max16=best['key'][0] <= 16)
    assert count == 2569 and best['key'][0] == 10
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
