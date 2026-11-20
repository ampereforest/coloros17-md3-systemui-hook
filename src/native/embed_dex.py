#!/usr/bin/env python3
"""Generate only a build-directory header; never modify the supplied dex."""
import pathlib
import sys

source, destination = map(pathlib.Path, sys.argv[1:3])
data = source.read_bytes()
if len(data) < 112 or data[:4] != b"dex\n":
    raise SystemExit(f"Not a valid standard DEX: {source}")
lines = ["#pragma once", "#include <cstddef>", "namespace md3 {",
         "alignas(16) inline constexpr unsigned char kDex[] = {"]
lines += ["    " + ",".join(f"0x{x:02x}" for x in data[i:i + 16]) + ","
          for i in range(0, len(data), 16)]
lines += ["};", "inline constexpr std::size_t kDexSize = sizeof(kDex);", "}", ""]
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text("\n".join(lines))
