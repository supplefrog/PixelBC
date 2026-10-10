#!/usr/bin/env python3
"""Exhaustive independent legal EAC feasibility for preserved BC3 failures.

Expectations come from BC rational interpolation and pinned, unmodified detex
modifier tables. This does not run or copy either candidate's fitting search.
"""
import collections
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = {
    'preserved_013_skew': '139a74bd084cebe81d9b2bac99c34236',
    'pixel_015_512_block10664': '40e473311d9583f2add42e84f025d089',
    'rejected_016_attempt1_block32320': '11a3dfacaf46084f612d6dbca131e121',
}


def enumerate_legal(source, tables):
    a, z = source[:2]
    bits = int.from_bytes(source[2:8], 'little')
    indices = [(bits >> (p * 3)) & 7 for p in range(16)]
    denominator = 7 if a > z else 5
    fractions = [(a, 1), (z, 1)]
    fractions += [((denominator - n) * a + n * z, denominator)
                  for n in range(1, denominator)]
    if a <= z:
        fractions += [(0, 1), (255, 1)]
    assert len(fractions) == 8
    values = [numerator // denominator for numerator, denominator in fractions]
    rounded = [(numerator + denominator // 2) // denominator
               for numerator, denominator in fractions]
    used = collections.Counter(values[i] for i in indices)
    exact = {n // d for i, (n, d) in enumerate(fractions)
             if i in indices and n in (0, 255 * d)}
    best = None
    count = feasible = 0
    for t, mod in enumerate(tables):
        for mult in range(1, 16):
            for base in range(256):
                palette = [max(0, min(255, base + mult * m)) for m in mod]
                if not exact.issubset(palette):
                    continue
                count += 1
                error = {v: min(abs(v - p) for p in palette) for v in used}
                key = (max(error.values()), sum(used[v] * e * e for v, e in error.items()))
                feasible += key[0] <= 16 and key[1] <= 16 * 64
                if best is None or key < best['key']:
                    best = dict(key=key, table=t, multiplier=mult, base=base,
                                palette=palette, error=error)
    return dict(source_BC=source.hex(), endpoints=[a, z], selector_indices=indices,
                rational_palette=fractions, independent_truncated_palette=values,
                production_015_rounded_palette=rounded, used_values_counts=dict(used),
                genuine_endpoint_values=sorted(exact), enumerated_legal_palettes=16 * 15 * 256,
                endpoint_preserving_palettes=count, palettes_meeting_max16_and_rms8=feasible,
                objective='minimum peak error then weighted SSE; genuine0/255 exact',
                minimum_peak_error=best['key'][0], optimal=best,
                oracle_modifier_source='unchanged pinned detex EAC table and integer clamp specification')


def main():
    text = (ROOT / 'tests/oracle/detex/decompress-eac.c').read_text()
    start = text.index('eac_modifier_table[16][8]')
    table = text[start:text.index('};', start)]
    tables = [list(map(int, re.findall(r'-?\d+', row)))
              for row in re.findall(r'\{([^{}]+)\}', table)]
    assert len(tables) == 16 and all(len(t) == 8 for t in tables)
    results = {name: enumerate_legal(bytes.fromhex(block), tables)
               for name, block in FIXTURES.items()}
    assert results['preserved_013_skew']['minimum_peak_error'] == 10
    assert all(r['palettes_meeting_max16_and_rms8'] for r in results.values())
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
