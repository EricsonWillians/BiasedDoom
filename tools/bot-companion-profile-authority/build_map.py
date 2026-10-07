#!/usr/bin/env python3
"""Pack the companion-profile authority UDMF fixture into a PWAD."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: build_map.py OUTPUT_WAD")

    textmap = Path(__file__).with_name("profile_authority.udmf").read_bytes()
    lumps = (("SQUADAUTH", b""), ("TEXTMAP", textmap), ("ENDMAP", b""))
    payload = bytearray()
    directory = bytearray()
    offset = 12
    for name, data in lumps:
        payload.extend(data)
        directory.extend(struct.pack("<II8s", offset, len(data), name.encode("ascii").ljust(8, b"\0")))
        offset += len(data)

    Path(sys.argv[1]).write_bytes(
        b"PWAD" + struct.pack("<II", len(lumps), offset) + payload + directory
    )


if __name__ == "__main__":
    main()
