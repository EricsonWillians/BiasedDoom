#!/usr/bin/env python3
"""Horizon-continuity metric for the sky-fog test (tools/test-sky-fog.sh).

The test map is a flat open plain under a sky ceiling, so with a level camera
the fog-saturated floor/wall meets the sky at the vertical center of the
frame. This tool measures the "seam": the mean per-channel color difference
between a band just above the horizon (sky + veil) and a band just below it
(fogged geometry). A physically consistent sky fog drives the seam toward
zero; an unfogged sky leaves a large seam.

Stdlib-only: reuses the PNG decoder from compare_screenshots.py.

Usage:
  analyze_sky_fog.py IMAGE [--above OFFSETS] [--below OFFSETS]

Prints:  SKYFOG METRIC image=<path> above=(r,g,b) below=(r,g,b) seam=<float>
Exit codes: 0 ok, 2 usage/decode error.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from compare_screenshots import PNGDecodeError, decode_png  # noqa: E402


def band_mean(data, width, bpp, y0, y1, x0, x1):
    sums = [0, 0, 0]
    count = 0
    for y in range(y0, y1):
        row = y * width * bpp
        for x in range(x0, x1):
            i = row + x * bpp
            sums[0] += data[i]
            sums[1] += data[i + 1]
            sums[2] += data[i + 2]
            count += 1
    if count == 0:
        raise ValueError("empty sample band")
    return [s / count for s in sums]


def horizon_seam(path, above=(-12, -3), below=(3, 12)):
    """Mean per-channel |sky - geometry| around the frame's vertical center.

    The bands hug the horizon: on a level camera over a flat plain the floor
    only reaches fog saturation within a few pixels of the vanishing line, so
    wider bands would measure partially fogged geometry instead of the seam.
    """
    width, height, bpp, data = decode_png(path)
    center = height // 2
    x0, x1 = width // 3, (2 * width) // 3
    a0, a1 = center + above[0], center + above[1]
    b0, b1 = center + below[0], center + below[1]
    if a0 < 0 or b1 > height:
        raise ValueError(f"sample bands out of range for {width}x{height} image")
    sky = band_mean(data, width, bpp, a0, a1, x0, x1)
    geo = band_mean(data, width, bpp, b0, b1, x0, x1)
    seam = sum(abs(s - g) for s, g in zip(sky, geo)) / 3.0
    return sky, geo, seam


def find_skyline_row(data, width, height, bpp, lo=None, hi=None):
    """Locate the sky/geometry boundary row: the strongest horizontal
    brightness edge near the frame's vertical center, measured on a control
    image (raw sky against fogged geometry makes the edge unambiguous)."""
    center = height // 2
    lo = center - 40 if lo is None else lo
    hi = center + 10 if hi is None else hi
    x0, x1 = width // 3, (2 * width) // 3
    rows = []
    for y in range(lo, hi + 1):
        rows.append(sum(band_mean(data, width, bpp, y, y + 1, x0, x1)) / 3.0)
    best_row, best_delta = lo, -1.0
    for i in range(len(rows) - 1):
        delta = abs(rows[i + 1] - rows[i])
        if delta > best_delta:
            best_delta = delta
            best_row = lo + i
    return best_row


def junction_seam(image_path, control_path):
    """Mean per-channel |sky - wall| across the sky/geometry boundary.

    The boundary row is detected on the control image and evaluated on the
    given one, so both on/off shots of the same static scene line up. This
    measures the actual horizon junction (veiled sky vs. the fogged wall it
    meets), not the natural floor-distance gradient.
    """
    cw, ch, cbpp, cdata = decode_png(control_path)
    width, height, bpp, data = decode_png(image_path)
    if (cw, ch) != (width, height):
        raise ValueError(f"image/control dimension mismatch: {width}x{height} vs {cw}x{ch}")
    boundary = find_skyline_row(cdata, cw, ch, cbpp)
    x0, x1 = width // 3, (2 * width) // 3
    sky = band_mean(data, width, bpp, boundary - 7, boundary - 2, x0, x1)
    wall = band_mean(data, width, bpp, boundary + 2, boundary + 7, x0, x1)
    seam = sum(abs(s - w) for s, w in zip(sky, wall)) / 3.0
    return boundary, sky, wall, seam


def main(argv):
    if len(argv) == 4 and argv[2] == "--boundary":
        try:
            boundary, sky, wall, seam = junction_seam(argv[1], argv[3])
        except (PNGDecodeError, ValueError) as exc:
            print(f"SKYFOG METRIC: ERROR {exc}")
            return 2
        print(
            "SKYFOG METRIC image=%s boundary=%d sky=(%.1f,%.1f,%.1f) wall=(%.1f,%.1f,%.1f) seam=%.2f"
            % (argv[1], boundary, sky[0], sky[1], sky[2], wall[0], wall[1], wall[2], seam)
        )
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    try:
        sky, geo, seam = horizon_seam(argv[1])
    except (PNGDecodeError, ValueError) as exc:
        print(f"SKYFOG METRIC: ERROR {exc}")
        return 2
    print(
        "SKYFOG METRIC image=%s above=(%.1f,%.1f,%.1f) below=(%.1f,%.1f,%.1f) seam=%.2f"
        % (argv[1], sky[0], sky[1], sky[2], geo[0], geo[1], geo[2], seam)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
