#!/usr/bin/env python3
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
if data[:6] != b"\x7fELF\x02\x01":
    raise SystemExit("Expected ELF64 little-endian")
header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
if header[2] != 183:
    raise SystemExit("Expected AArch64 machine")
offset, entry_size, count = header[5], header[9], header[10]
loads = []
for i in range(count):
    segment = struct.unpack_from("<IIQQQQQQ", data, offset + i * entry_size)
    if segment[0] == 1:
        _, flags, fileoffset, vaddr, _, _, _, alignment = segment
        if alignment < 16384 or (fileoffset - vaddr) % 16384:
            raise SystemExit(f"Invalid 16k PT_LOAD alignment: {segment}")
        loads.append(alignment)
if not loads:
    raise SystemExit("ELF has no PT_LOAD segments")
print(f"Verified ARM64 ELF: {len(loads)} PT_LOAD segments aligned >=16k")
