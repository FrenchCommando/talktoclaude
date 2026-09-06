"""Renders talktoclaude.ico (16/24/32/48/64/256) from the same design as
site/icon.svg: dark rounded square, red dot, two sound arcs. Standard
library only, so it runs anywhere; the output is committed and only needs
regenerating if the design changes.

    python tools/make_icon.py
"""
import math
import struct
from pathlib import Path

BG = (0x1E, 0x1E, 0x1C)
DOT = (0xDC, 0x3C, 0x32)
ARC = (0xD8, 0xD4, 0xC8)
SIZES = (16, 24, 32, 48, 64, 256)
SUPERSAMPLE = 4


def coverage(x, y, size):
    """Premultiplied (r, g, b, a) for pixel (x, y) at this size, in the
    64-unit design space of icon.svg."""
    s = 64.0 / size
    r = g = b = a = 0.0
    n = SUPERSAMPLE * SUPERSAMPLE
    for sy in range(SUPERSAMPLE):
        for sx in range(SUPERSAMPLE):
            px = (x + (sx + 0.5) / SUPERSAMPLE) * s
            py = (y + (sy + 0.5) / SUPERSAMPLE) * s
            c = sample(px, py)
            if c is None:
                continue
            r += c[0]; g += c[1]; b += c[2]; a += 1
    if a == 0:
        return (0, 0, 0, 0)
    return (int(r / n), int(g / n), int(b / n), int(255 * a / n))


def sample(px, py):
    # Rounded square, corner radius 14, inset 2.
    ix = min(max(px, 16.0), 48.0)
    iy = min(max(py, 16.0), 48.0)
    if math.hypot(px - ix, py - iy) > 14.0:
        return None
    if px < 2 or px > 62 or py < 2 or py > 62:
        return None
    # Dot.
    if math.hypot(px - 32, py - 32) <= 14.0:
        return DOT
    # Arcs, matching icon.svg's paths: "M46 20 a17 17 0 0 1 0 24" is a
    # radius-17 arc between (46,20) and (46,44), so its centre is at
    # x = 46 - sqrt(17^2 - 12^2); likewise the radius-25 arc between
    # (52,14) and (52,50). Stroke width 3, rounded ends approximated.
    for radius, cx, half_chord, alpha in ((17.0, 46 - math.sqrt(17**2 - 12**2), 12.0, 0.8),
                                          (25.0, 52 - math.sqrt(25**2 - 18**2), 18.0, 0.45)):
        d = math.hypot(px - cx, py - 32)
        if abs(d - radius) <= 1.5 and px > cx and abs(py - 32) <= half_chord + 1.0:
            return tuple(int(BG[i] + (ARC[i] - BG[i]) * alpha) for i in range(3))
    return BG


def image(size):
    px = [coverage(x, y, size) for y in range(size) for x in range(size)]
    # BMP in an ICO: 32bpp BGRA, bottom-up rows, plus an empty AND mask.
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            r, g, b, a = px[y * size + x]
            # Un-premultiply for BGRA-with-alpha as Windows expects.
            if a:
                r, g, b = (min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_row = b"\x00" * (((size + 31) // 32) * 4)
    return header + b"".join(rows) + mask_row * size


def main():
    images = [(s, image(s)) for s in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    dest = Path(__file__).resolve().parent.parent / "talktoclaude.ico"
    dest.write_bytes(out)
    print(f"wrote {dest} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
