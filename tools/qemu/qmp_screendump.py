#!/usr/bin/env python3
"""
QMP helper: take a VGA screendump (PPM) from a running QEMU instance and
verify its contents. Used by the KMSDRM QEMU test harness.

Usage:
    qmp_screendump.py [--device ID] [--head N] <qmp_socket> <out.ppm>
    qmp_screendump.py --check-bands [--device ID] [--head N] <qmp_socket> <out.ppm>

--check-bands additionally verifies the canonical SDLop KMSDRM test pattern:
three horizontal bands - dark red on top, dark green in the middle, dark blue
on the bottom (0xC0 channel intensity), each covering a third of the image.
Exit code 0 = pattern matched, 1 = mismatch, 2 = screendump/IO error.
"""
import json
import re
import socket
import sys


def qmp_command(sock_path, execute, arguments=None):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(sock_path)
    f = s.makefile("rw", encoding="utf-8")
    greeting = json.loads(f.readline())
    try:
        f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
        f.flush()
        f.readline()
        cmd = {"execute": execute}
        if arguments is not None:
            cmd["arguments"] = arguments
        f.write(json.dumps(cmd) + "\n")
        f.flush()
        for _ in range(20):
            line = f.readline()
            if not line:
                break
            msg = json.loads(line)
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                raise RuntimeError(f"QMP error: {msg['error']}")
        raise RuntimeError("no return from QMP command")
    finally:
        try:
            f.close()
        except Exception:
            pass
        s.close()


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n<maxval>\n<binary>
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise RuntimeError("not a P6 PPM file")
    width, height, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxval != 255:
        raise RuntimeError("maxval != 255")
    offset = m.end()
    pix = data[offset:offset + width * height * 3]
    if len(pix) < width * height * 3:
        raise RuntimeError("truncated pixel data")
    return width, height, pix


def band_color(pix, width, y0, y1):
    r = g = b = n = 0
    for y in range(y0, y1):
        row = y * width * 3
        for x in range(0, width, 7):  # sample every 7th pixel; plenty
            r += pix[row + x * 3]
            g += pix[row + x * 3 + 1]
            b += pix[row + x * 3 + 2]
            n += 1
    return (r // n, g // n, b // n)


def main():
    args = sys.argv[1:]
    check_bands = False
    device = None
    head = None
    while args:
        if args[0] == "--check-bands":
            check_bands = True
            args = args[1:]
        elif args[0] == "--device" and len(args) >= 2:
            device = args[1]
            args = args[2:]
        elif args[0] == "--head" and len(args) >= 2:
            head = int(args[1])
            args = args[2:]
        else:
            break
    if len(args) < 2:
        print(__doc__)
        return 2
    sock_path, ppm_path = args[0], args[1]

    screendump_args = {"filename": ppm_path}
    if device is not None:
        screendump_args["device"] = device
    if head is not None:
        screendump_args["head"] = head
    try:
        qmp_command(sock_path, "screendump", screendump_args)
    except Exception as e:
        print(f"screendump failed: {e}", file=sys.stderr)
        return 2

    if not check_bands:
        return 0

    try:
        width, height, pix = read_ppm(ppm_path)
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
    if ok_top and ok_mid and ok_bot:
        print("band pattern: MATCH (KMS scanout verified)")
        return 0
    print(f"band pattern: MISMATCH (top={'ok' if ok_top else 'bad'} mid={'ok' if ok_mid else 'bad'} bot={'ok' if ok_bot else 'bad'})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
