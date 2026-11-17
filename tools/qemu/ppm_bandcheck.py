#!/usr/bin/env python3
"""Verify the SDLop KMSDRM band pattern in a binary PPM (same logic as qmp_screendump.py --check-bands)."""
import re
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise RuntimeError("not a P6 PPM file")
    width, height, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxval != 255:
        raise RuntimeError("maxval != 255")
    pix = data[m.end():m.end() + width * height * 3]
    if len(pix) < width * height * 3:
        raise RuntimeError("truncated pixel data")
    return width, height, pix


def band_color(pix, width, y0, y1):
    r = g = b = n = 0
    for y in range(y0, y1):
        row = y * width * 3
        for x in range(0, width, 7):
            r += pix[row + x * 3]
            g += pix[row + x * 3 + 1]
            b += pix[row + x * 3 + 2]
            n += 1
    return (r // n, g // n, b // n)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        width, height, pix = read_ppm(sys.argv[1])
    except Exception as e:
        print(f"PPM parse failed: {e}", file=sys.stderr)
        return 2
    third = height // 3
    top = band_color(pix, width, height // 8, third - height // 8)
    mid = band_color(pix, width, third + height // 8, 2 * third - height // 8)
    bot = band_color(pix, width, 2 * third + height // 8, height - height // 8)
    print(f"screendump {width}x{height}: top={top} mid={mid} bot={bot}")

    def close(c, target, tol=40):
        return abs(c[0] - target[0]) <= tol and abs(c[1] - target[1]) <= tol and abs(c[2] - target[2]) <= tol

    ok_top = close(top, (0xC0, 0x00, 0x00))
    ok_mid = close(mid, (0x00, 0xC0, 0x00))
    ok_bot = close(bot, (0x00, 0x00, 0xC0))
    print(f"band pattern: {'MATCH' if (ok_top and ok_mid and ok_bot) else 'MISMATCH'}")
    return 0 if (ok_top and ok_mid and ok_bot) else 1


if __name__ == "__main__":
    sys.exit(main())
