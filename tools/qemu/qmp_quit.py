#!/usr/bin/env python3
"""QMP helper: power a QEMU guest off from the host.

Safety net for the unattended installer: if the "Installation complete"
dialog appears anyway (e.g. a preseed regressed), the installer is done
writing to disk and the guest is just sitting on a modal prompt we cannot
answer from the write-only serial log. Quitting host-side is safe then.

Usage:
    qmp_quit.py <qmp_socket>

Exit code 0 = quit acknowledged, non-zero = QMP/IO error.
"""
import json
import socket
import sys


def main():
    if len(sys.argv) != 2:
        print("usage: qmp_quit.py <qmp_socket>", file=sys.stderr)
        return 2
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sys.argv[1])
    f = s.makefile("rw", encoding="utf-8")
    f.readline()  # greeting
    try:
        f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
        f.flush()
        f.readline()
        f.write(json.dumps({"execute": "quit"}) + "\n")
        f.flush()
        line = f.readline()
    finally:
        try:
            f.close()
        except Exception:
            pass
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
