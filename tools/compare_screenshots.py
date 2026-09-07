#!/usr/bin/env python3
"""Golden-screenshot pixel comparison for the BiasedDoom Python test harness.

Compares a captured frame against a checked-in golden image. Intentionally
stdlib-only (no Pillow): PNGs are decoded/encoded with zlib + struct.

Supported input: PNG, bit depth 8, color type 2 (RGB) or 6 (RGBA),
non-interlaced, any of the standard scanline filters 0-4. This matches the
engine's screenshot output (M_CreatePNG/M_SaveBitmap in
src/common/textures/m_png.cpp, which writes 8-bit RGB non-interlaced PNGs).

Usage:
  compare_screenshots.py GOLDEN ACTUAL [--threshold FLOAT]
      [--max-diff-pct FLOAT] [--diff-out PATH]

Exit codes:
  0  images match within tolerance (PASS)
  1  images differ beyond tolerance, or dimension/format mismatch (FAIL)
  2  usage error or undecodable/unsupported input
"""

import argparse
import struct
import sys
import zlib

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {2: 3, 6: 4}  # color type -> bytes per pixel


class PNGDecodeError(Exception):
    """Raised for malformed or unsupported PNG input."""


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path):
    """Decode a PNG file. Returns (width, height, channels, bytes).

    `bytes` is a bytes object of length width * height * channels in
    row-major RGB/RGBA order. Raises PNGDecodeError on any problem.
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise PNGDecodeError(f"{path}: cannot read file: {exc}")

    if len(data) < 8 or data[:8] != PNG_MAGIC:
        raise PNGDecodeError(f"{path}: not a PNG file (bad magic)")

    width = height = None
    bit_depth = color_type = interlace = None
    idat = bytearray()

    off = 8
    while off + 8 <= len(data):
        (length,) = struct.unpack(">I", data[off:off + 4])
        ctype = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + length]
        if len(body) != length:
            raise PNGDecodeError(f"{path}: truncated {ctype.decode('ascii', 'replace')} chunk")
        if ctype == b"IHDR":
            (width, height, bit_depth, color_type, _comp,
             _filt, interlace) = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        off += 12 + length

    if width is None or not idat:
        raise PNGDecodeError(f"{path}: missing IHDR or IDAT chunk")
    if bit_depth != 8:
        raise PNGDecodeError(f"{path}: unsupported bit depth {bit_depth} (need 8)")
    if color_type not in _CHANNELS:
        raise PNGDecodeError(
            f"{path}: unsupported color type {color_type} (need 2=RGB or 6=RGBA)")
    if interlace != 0:
        raise PNGDecodeError(f"{path}: interlaced PNGs are not supported")

    bpp = _CHANNELS[color_type]
    stride = width * bpp
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as exc:
        raise PNGDecodeError(f"{path}: zlib decompression failed: {exc}")
    if len(raw) != height * (stride + 1):
        raise PNGDecodeError(
            f"{path}: decompressed size {len(raw)} does not match "
            f"{height}x{width}x{bpp} scanlines")

    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        ftype = raw[y * (stride + 1)]
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for x in range(bpp, stride):
                row[x] = (row[x] + row[x - bpp]) & 0xFF
        elif ftype == 2:  # Up
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif ftype == 3:  # Average
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                c = prev[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + _paeth(a, prev[x], c)) & 0xFF
        else:
            raise PNGDecodeError(f"{path}: unknown filter type {ftype} on row {y}")
        out[y * stride:(y + 1) * stride] = row
        prev = row

    return width, height, bpp, bytes(out)


def _chunk(ctype, body):
    return (struct.pack(">I", len(body)) + ctype + body
            + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))


def encode_gray_png(width, height, pixels):
    """Encode an 8-bit grayscale PNG (color type 0, filter 0)."""
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        rows += pixels[y * width:(y + 1) * width]
    return PNG_MAGIC + _chunk(b"IHDR", ihdr) \
        + _chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + _chunk(b"IEND", b"")


def compare(golden_path, actual_path, threshold, max_diff_pct, diff_out):
    """Returns (ok, report_lines). Raises PNGDecodeError on bad input."""
    gw, gh, gc, gdata = decode_png(golden_path)
    aw, ah, ac, adata = decode_png(actual_path)

    header = (f"golden={golden_path} ({gw}x{gh}x{gc}) "
              f"actual={actual_path} ({aw}x{ah}x{ac})")

    if (gw, gh) != (aw, ah):
        return False, [
            f"dimension mismatch: golden is {gw}x{gh}, actual is {aw}x{ah} :: {header}"
        ]
    if gc != ac:
        return False, [
            f"format mismatch: golden has {gc} channels, actual has {ac} :: {header}"
        ]

    npix = gw * gh
    bpp = gc
    mismatched = 0
    err_sum = 0
    worst = 0
    diff_map = bytearray(npix) if diff_out else None

    for i in range(npix):
        base = i * bpp
        pix_bad = False
        for c in range(bpp):
            d = abs(gdata[base + c] - adata[base + c])
            err_sum += d
            if d > worst:
                worst = d
            if d > threshold:
                pix_bad = True
        if pix_bad:
            mismatched += 1
            if diff_map is not None:
                diff_map[i] = 255

    diff_pct = mismatched * 100.0 / npix
    mean_err = err_sum / float(npix * bpp)
    ok = diff_pct <= max_diff_pct

    report = [
        f"dimensions={gw}x{gh} channels={bpp} "
        f"mismatched_pixels={mismatched}/{npix} ({diff_pct:.4f}%) "
        f"mean_abs_channel_err={mean_err:.4f} worst_channel_delta={worst} "
        f"threshold={threshold:g} max_diff_pct={max_diff_pct:g} :: {header}"
    ]

    if diff_out:
        try:
            with open(diff_out, "wb") as f:
                f.write(encode_gray_png(gw, gh, bytes(diff_map)))
            report.append(f"diff map written to {diff_out}")
        except OSError as exc:
            report.append(f"warning: could not write diff map {diff_out}: {exc}")

    return ok, report


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare an actual screenshot against a golden image "
                    "(stdlib-only PNG decode/encode; 8-bit RGB/RGBA "
                    "non-interlaced PNGs).")
    parser.add_argument("golden", help="checked-in reference PNG")
    parser.add_argument("actual", help="newly captured PNG")
    parser.add_argument("--threshold", type=float, default=8.0,
                        help="per-channel absolute tolerance, 0-255 "
                             "(default: 8)")
    parser.add_argument("--max-diff-pct", type=float, default=0.5,
                        help="percent of pixels allowed to exceed --threshold "
                             "(default: 0.5)")
    parser.add_argument("--diff-out", metavar="PATH",
                        help="write a grayscale diff map PNG "
                             "(white = diff beyond threshold)")
    args = parser.parse_args(argv)

    if args.threshold < 0 or args.threshold > 255:
        parser.error("--threshold must be between 0 and 255")
    if args.max_diff_pct < 0 or args.max_diff_pct > 100:
        parser.error("--max-diff-pct must be between 0 and 100")

    try:
        ok, report = compare(args.golden, args.actual, args.threshold,
                             args.max_diff_pct, args.diff_out)
    except PNGDecodeError as exc:
        print(f"GOLDEN TEST: ERROR {exc}")
        return 2

    verdict = "PASS" if ok else "FAIL"
    for line in report:
        print(f"GOLDEN TEST: {verdict} {line}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
