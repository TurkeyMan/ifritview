#!/usr/bin/env python3
"""Keep the per-pixel detail program separate from its per-image table solve.

DXBC budgets are deterministic across hardware; wall-clock driver compilation
is not. The limits leave compiler headroom above the 312-instruction,
10,992-byte pixel program, while rejecting both the former 1,418-instruction
shaping branch and the 8,831-instruction combined table program.
"""
import struct
import sys
from pathlib import Path

# Bytecode bytes, instruction slots (FXC ps_5_0 /O3 STAT chunk).
BUDGETS = {"detail": (16 * 1024, 512)}


def instructions(data):
    if len(data) < 32 or data[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    version, size, chunks = struct.unpack_from("<III", data, 20)
    if version != 1 or size != len(data) or chunks > (len(data) - 32) // 4:
        raise ValueError("invalid DXBC header")
    for index in range(chunks):
        offset = struct.unpack_from("<I", data, 32 + 4 * index)[0]
        if offset + 8 > len(data):
            raise ValueError("invalid DXBC chunk")
        length = struct.unpack_from("<I", data, offset + 4)[0]
        if offset + 8 + length > len(data):
            raise ValueError("truncated DXBC chunk")
        if data[offset:offset + 4] == b"STAT" and length >= 4:
            return struct.unpack_from("<I", data, offset + 8)[0]
    raise ValueError("no shader statistics")


def main(directory):
    failed = False
    for name, (max_bytes, max_instructions) in BUDGETS.items():
        path = directory / (name + ".dxbc")
        try:
            data = path.read_bytes()
            count = instructions(data)
            ok = len(data) <= max_bytes and count <= max_instructions
            print(f"{name}: {len(data)}/{max_bytes} bytes, "
                  f"{count}/{max_instructions} instructions: "
                  f"{'ok' if ok else 'FAILED shader budget'}")
            failed |= not ok
        except (OSError, ValueError, struct.error) as error:
            print(f"{name}: FAILED shader budget: {error}", file=sys.stderr)
            failed = True
    return int(failed)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check-shader-budget.py BUILD_SHADER_DIRECTORY")
    sys.exit(main(Path(sys.argv[1])))
