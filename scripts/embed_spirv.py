#!/usr/bin/env python3
"""Embed validated little-endian SPIR-V without native-endian assumptions."""
import pathlib
import re
import struct
import sys
data = pathlib.Path(sys.argv[1]).read_bytes()
assert len(data) % 4 == 0 and data[:4] == b"\x03\x02\x23\x07"
words = struct.unpack("<" + "I" * (len(data) // 4), data)
symbol = sys.argv[3] if len(sys.argv) > 3 else "bc_decode_spv"
assert re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol)
pathlib.Path(sys.argv[2]).write_text(
    "#pragma once\n#include <cstdint>\nstatic const uint32_t " + symbol + "[] = {\n"
    + ",\n".join("0x%08xu" % word for word in words) + "\n};\n"
)
