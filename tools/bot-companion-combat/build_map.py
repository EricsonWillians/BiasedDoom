#!/usr/bin/env python3
"""Pack each companion-combat UDMF source into its own map PWAD."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def write_map(output: Path, map_name: str, source_name: str) -> None:
    textmap = Path(__file__).with_name(source_name).read_bytes()
    lumps = ((map_name, b""), ("TEXTMAP", textmap), ("ENDMAP", b""))
    payload = bytearray()
    directory = bytearray()
    offset = 12
    for name, data in lumps:
        payload.extend(data)
        directory.extend(struct.pack("<II8s", offset, len(data), name.encode("ascii").ljust(8, b"\0")))
        offset += len(data)

    output.write_bytes(b"PWAD" + struct.pack("<II", len(lumps), offset) + payload + directory)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: build_map.py OUTPUT_WAD")

    output = Path(sys.argv[1])
    write_map(output, "BOTFIGHT", "botfight.udmf")
    write_map(output.with_name("botsfe.wad"), "BOTSFE", "botsafety.udmf")


if __name__ == "__main__":
    main()
