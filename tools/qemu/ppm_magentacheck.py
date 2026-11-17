#!/usr/bin/env python3
"""Exit 0 when a binary PPM's mean color looks like the GL test's magenta."""
import sys

def main():
    path = sys.argv[1]
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        if magic != b'P6':
            print('not a binary ppm', file=sys.stderr)
            return 2
        dims = f.readline().split()
        while dims and dims[0].startswith(b'#'):
            dims = f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        maxv = int(f.readline())
        header = f.tell()
        px = f.read(w * h * 3)
    if len(px) < w * h * 3:
        print('truncated ppm', file=sys.stderr)
        return 2
    n = w * h
    mr = sum(px[0::3]) / n
    mg = sum(px[1::3]) / n
    mb = sum(px[2::3]) / n
    print(f'  gl screendump {w}x{h}: mean rgb=({mr:.0f}, {mg:.0f}, {mb:.0f}) maxv={maxv}')
    # test clears to (0.9, 0.1, 0.5) -> ~(230, 26, 128)
    return 0 if (mr > 120 and mb > 60 and mg < 90) else 1

if __name__ == '__main__':
    sys.exit(main())
