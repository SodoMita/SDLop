#!/usr/bin/env python3
"""
SDLop test harness: mini X11 server (pure Python, no dependencies).

Implements just enough of the core X11 protocol for the SDLop X11 driver:
connection setup, window/GC/pixmap/cursor/colormap lifecycle, atoms and
properties, input focus, grabs (always succeed), PutImage (frames counted),
BIG-REQUESTS, and a tiny WM emulation (_NET_WM_STATE and WM_CHANGE_STATE
client messages, WM_DELETE_WINDOW injection).

It exists because some CI/sandbox environments have no Xvfb, and it doubles
as a deterministic input injector: with --script basic_input, once the
client's first top-level window is mapped it sends a fixed event sequence
(focus, enter, motion, key press/release incl. a server auto-repeat pair,
buttons, wheel, WM_DELETE_WINDOW).

Usage: mini_xserver.py [--display N] [--script basic_input|linger]
Prints "READY <display>" on stdout once listening; on client disconnect
prints "MINI-X11-SERVER-STATS frames_total=N".

This software is provided 'as-is', without any express or implied warranty
(zlib license, see LICENSE).
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

PREDEFINED_ATOMS = {
    "PRIMARY": 1, "SECONDARY": 2, "ARC": 3, "ATOM": 4, "BITMAP": 5,
    "CARDINAL": 6, "COLORMAP": 7, "CURSOR": 8, "CUT_BUFFER0": 9,
    "DRAWABLE": 16, "FONT": 18, "INTEGER": 19, "PIXMAP": 20,
    "POINT": 21, "RECTANGLE": 22, "RESOURCE_MANAGER": 23, "RGB_COLOR_MAP": 24,
    "STRING": 31, "VISUALID": 32, "WINDOW": 33, "WM_COMMAND": 34,
    "WM_HINTS": 35, "WM_CLIENT_MACHINE": 36, "WM_ICON_NAME": 37,
    "WM_ICON_SIZE": 38, "WM_NAME": 39, "WM_NORMAL_HINTS": 40,
    "WM_SIZE_HINTS": 41, "WM_ZOOM_HINTS": 42, "MIN_WIDTH": 46,
    "MIN_HEIGHT": 47, "MAX_WIDTH": 48, "MAX_HEIGHT": 49,
    "WM_STATE": 65, "WM_CLASS": 67, "WM_TRANSIENT_FOR": 68,
}

KEY_PRESS, KEY_RELEASE, BUTTON_PRESS, BUTTON_RELEASE, MOTION_NOTIFY = 2, 3, 4, 5, 6
ENTER_NOTIFY, LEAVE_NOTIFY, FOCUS_IN, FOCUS_OUT = 7, 8, 9, 10
EXPOSE, CREATE_NOTIFY, DESTROY_NOTIFY = 12, 16, 17
UNMAP_NOTIFY, MAP_NOTIFY, CONFIGURE_NOTIFY = 18, 19, 22
PROPERTY_NOTIFY, CLIENT_MESSAGE = 28, 33

EXPOSURE_MASK = 1 << 15
POINTER_MOTION_MASK = 1 << 6
PROPERTY_CHANGE_MASK = 1 << 22

ERR_REQUEST, ERR_VALUE, ERR_WINDOW, ERR_PIXMAP, ERR_ATOM = 1, 2, 3, 4, 5
ERR_DRAWABLE, ERR_IMPLEMENTATION = 9, 17

BIG_REQUESTS_OPCODE = 133

NOTIFY_NORMAL = 0
NOTIFY_ANCESTOR = 0
NOTIFY_NONLINEAR = 3

VISUAL_ID = 0x220
ROOT_ID = 0x209
COLORMAP_ID = 0x210


def u16(v):
    return struct.pack("<H", v & 0xFFFF)


def s16(v):
    return struct.pack("<h", v)


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def pad4(data):
    return data + b"\0" * ((-len(data)) % 4)


class MiniXServer:
    def __init__(self, display, width, height, script):
        self.display_num = display
        self.width = width
        self.height = height
        self.script = script
        self.listeners = []
        self.client = None
        self.inbuf = b""
        self.seq = 0
        self.next_atom = 1000
        self.atoms = dict(PREDEFINED_ATOMS)
        self.atom_names = {v: k for k, v in PREDEFINED_ATOMS.items()}
        self.windows = {}
        self.properties = {}
        self.focus_window = 0
        self.pointer_root = (0, 0)
        self.frames_total = 0
        self.script_started = False
        self.script_events = []
        self.start_time = time.monotonic()
        self.bigreq_enabled = False

    # -- sockets -----------------------------------------------------------

    def listen(self):
        path = "/tmp/.X11-unix/X%d" % self.display_num
        try:
            os.makedirs("/tmp/.X11-unix", exist_ok=True)
        except OSError:
            pass
        bound = False
        for addr in (path, "\0" + path):  # filesystem socket + abstract namespace
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                if not addr.startswith("\0"):
                    try:
                        os.unlink(addr)
                    except OSError:
                        pass
                s.bind(addr)
                s.listen(4)
                s.setblocking(False)
                self.listeners.append(s)
                bound = True
            except OSError as e:
                sys.stderr.write("mini_xserver: cannot bind %r: %s\n" % (addr, e))
                s.close()
        if not bound:
            sys.stderr.write("mini_xserver: no socket to listen on\n")
            sys.exit(2)

    # -- wire helpers --------------------------------------------------------

    def send_reply(self, data1, body24, extra=b""):
        assert len(body24) == 24, "reply body must be 24 bytes"
        self.client.sendall(
            struct.pack("<BBHI", 1, data1, self.seq & 0xFFFF, len(extra) // 4)
            + body24 + pad4(extra))

    def send_error(self, code, badvalue=0, minor=0, major=0):
        if not self.client:
            return
        self.client.sendall(
            struct.pack("<BBHIHB", 0, code, self.seq & 0xFFFF, badvalue,
                        minor, major) + b"\0" * 21)

    def send_event(self, code, detail, body28):
        assert len(body28) == 28
        if self.client:
            self.client.sendall(struct.pack("<BBH", code, detail, self.seq & 0xFFFF) + body28)

    def server_time(self):
        return int((time.monotonic() - self.start_time) * 1000) & 0xFFFFFFFF

    # -- event constructors ---------------------------------------------------

    def ev_buttonlike(self, code, detail, win, x, y, state=0, t=None):
        body = (u32(self.server_time() if t is None else t) + u32(ROOT_ID)
                + u32(win["id"]) + u32(0)
                + s16(win["x"] + x) + s16(win["y"] + y) + s16(x) + s16(y)
                + u16(state) + b"\x01\x00")
        self.send_event(code, detail, body)

    def ev_crossing(self, code, win, x, y):
        body = (u32(self.server_time()) + u32(ROOT_ID) + u32(win["id"]) + u32(0)
                + s16(win["x"] + x) + s16(win["y"] + y) + s16(x) + s16(y)
                + u16(0) + struct.pack("BB", NOTIFY_NORMAL, 0x03))  # same_screen=1 focus=1
        self.send_event(code, NOTIFY_ANCESTOR, body)

    def ev_focus(self, code, win_id):
        body = u32(win_id) + struct.pack("B", NOTIFY_NORMAL) + b"\0" * 23
        self.send_event(code, NOTIFY_NONLINEAR, body)

    def ev_expose(self, win, x, y, w, h, count=0):
        body = (u32(win["id"]) + u16(x) + u16(y) + u16(w) + u16(h) + u16(count)
                + b"\0" * 14)
        self.send_event(EXPOSE, 0, body)

    def ev_configure(self, win):
        body = (u32(win["id"]) + u32(win["id"]) + u32(0)
                + s16(win["x"]) + s16(win["y"]) + u16(win["w"]) + u16(win["h"])
                + u16(0) + struct.pack("BB", 1 if win.get("override_redirect") else 0, 0)
                + b"\0" * 4)
        self.send_event(CONFIGURE_NOTIFY, 0, body)

    def ev_map(self, win):
        self.send_event(MAP_NOTIFY, 0, u32(win["id"]) + u32(win["id"]) + b"\0" * 20)

    def ev_unmap(self, win):
        self.send_event(UNMAP_NOTIFY, 0, u32(win["id"]) + u32(win["id"]) + b"\0" * 20)

    def ev_destroy(self, win_id):
        self.send_event(DESTROY_NOTIFY, 0, u32(win_id) + u32(win_id) + b"\0" * 20)

    def ev_property(self, win_id, atom, state=0):
        if not atom:
            return
        body = (u32(win_id) + u32(atom) + u32(self.server_time())
                + struct.pack("B", state) + b"\0" * 15)
        self.send_event(PROPERTY_NOTIFY, 0, body)

    def ev_client_message(self, win_id, msg_type, data20):
        self.send_event(CLIENT_MESSAGE, 32, u32(win_id) + u32(msg_type) + data20)

    # -- connection setup ------------------------------------------------------

    @staticmethod
    def recv_exact(conn, n):
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                return b""
            data += chunk
        return data

    def handle_setup(self, conn):
        hdr = self.recv_exact(conn, 12)
        if len(hdr) < 12 or hdr[0:1] != b"l":
            conn.sendall(struct.pack("<BBH", 0, 11, 0) + struct.pack("<H", 0))
            return False
        name_len, data_len = struct.unpack_from("<HH", hdr, 6)
        self.recv_exact(conn, name_len + (-name_len) % 4 + data_len + (-data_len) % 4)

        vendor = b"SDLop mini X"
        pixmap_formats = (struct.pack("<BBBB4x", 1, 1, 32, 0)
                          + struct.pack("<BBBB4x", 24, 32, 32, 0))
        root_screen = (u32(ROOT_ID) + u32(COLORMAP_ID)
                       + u32(0xFFFFFF) + u32(0x000000) + u32(0)          # white, black, input masks
                       + u16(self.width) + u16(self.height)
                       + u16(int(self.width * 25.4 / 96)) + u16(int(self.height * 25.4 / 96))
                       + u16(1) + u16(1) + u32(VISUAL_ID)
                       + struct.pack("BBBB", 1, 0, 24, 1))                # backing, save, root depth, 1 depth
        depth24 = struct.pack("<BBH4x", 24, 0, 1)
        visual = (u32(VISUAL_ID) + struct.pack("<BBH", 4, 8, 256)
                  + u32(0xFF0000) + u32(0x00FF00) + u32(0x0000FF) + b"\0" * 4)

        body = (u32(12100007)     # release number
                + u32(0x200000)   # resource-id base
                + u32(0x1FFFFF)   # resource-id mask
                + u32(256)        # motion buffer size
                + u16(len(vendor))
                + u16(65535)      # maximum-request-length (pre BIG-REQUESTS)
                + struct.pack("BBBB", 1, 2, 8, 255)   # screens, formats, min/max keycode
                + struct.pack("BBBB", 32, 32, 8, 255)
                + b"\0" * 4)
        rest = body + pad4(vendor) + pixmap_formats + root_screen + depth24 + visual
        conn.sendall(struct.pack("<BBHHH", 1, 0, 11, 0, len(rest) // 4) + rest)
        return True

    # -- main loop --------------------------------------------------------------

    def serve(self):
        self.listen()
        print("READY %d" % self.display_num, flush=True)
        watchdog = time.monotonic() + 60
        while time.monotonic() < watchdog:
            fds = list(self.listeners) + ([self.client] if self.client else [])
            timeout = 0.05
            if self.script_events:
                timeout = max(0.0, min(timeout, self.script_events[0][0] - time.monotonic()))
            try:
                r, _, _ = select.select(fds, [], [], timeout)
            except OSError:
                r = []
            now = time.monotonic()
            while self.script_events and self.script_events[0][0] <= now:
                _, fn = self.script_events.pop(0)
                try:
                    fn()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    self.client_gone()
                    return 0
            for fd in r:
                if fd in self.listeners:
                    conn, _ = fd.accept()
                    if self.client is not None:
                        conn.close()  # one test client at a time
                        continue
                    conn.setblocking(True)
                    if self.handle_setup(conn):
                        conn.setblocking(False)
                        self.client = conn
                        self.inbuf = b""
                        self.seq = 0
                    else:
                        conn.close()
                else:
                    try:
                        data = fd.recv(1 << 20)
                    except (ConnectionResetError, BrokenPipeError):
                        data = b""
                    if not data:
                        self.client_gone()
                        return 0
                    self.inbuf += data
                    try:
                        self.process_buffer()
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        self.client_gone()
                        return 0
        sys.stderr.write("mini_xserver: watchdog expired\n")
        return 1

    def client_gone(self):
        if self.client:
            try:
                self.client.close()
            except OSError:
                pass
        self.client = None
        print("MINI-X11-SERVER-STATS frames_total=%d" % self.frames_total, flush=True)

    def process_buffer(self):
        while True:
            if len(self.inbuf) < 4:
                return
            length = struct.unpack_from("<H", self.inbuf, 2)[0]
            header = 4
            if length == 0 and self.bigreq_enabled:
                if len(self.inbuf) < 8:
                    return
                length = struct.unpack_from("<I", self.inbuf, 4)[0]
                header = 8
            total = length * 4
            if total < header or len(self.inbuf) < total:
                return
            req = self.inbuf[:total]
            self.inbuf = self.inbuf[total:]
            self.seq += 1
            if os.environ.get("MINI_X11_DEBUG"):
                sys.stderr.write("mini_xserver: req seq=%d opcode=%d len=%d hex=%s\n"
                                 % (self.seq, req[0], total, req[:32].hex()))
            self.dispatch(req[0], req)

    def dispatch(self, opcode, req):
        if opcode == BIG_REQUESTS_OPCODE or (opcode == 0 and len(req) == 4):
            # BigReqEnable. (Modern libX11 speaks XCB underneath; xcb emits
            # the enable with a zero opcode byte on some setups - a bare
            # 4-byte opcode-0 request is otherwise impossible, so this is
            # unambiguous in practice.)
            self.bigreq_enabled = True
            self.send_reply(0, u32(4194303) + b"\0" * 20)
            return
        handler = self.REQUESTS.get(opcode)
        if handler is None:
            sys.stderr.write("mini_xserver: unknown opcode %d (seq %d)\n" % (opcode, self.seq))
            self.send_error(ERR_REQUEST, major=opcode)
            return
        try:
            handler(self, req)
        except Exception as e:
            sys.stderr.write("mini_xserver: error in opcode %d: %r\n" % (opcode, e))
            self.send_error(ERR_IMPLEMENTATION, major=opcode)

    # -- windows -----------------------------------------------------------------

    def req_create_window(self, req):
        depth = req[1]
        wid = struct.unpack_from("<I", req, 4)[0]
        parent = struct.unpack_from("<I", req, 8)[0]
        x, y = struct.unpack_from("<hh", req, 12)
        w, h, border, wclass = struct.unpack_from("<HHHH", req, 16)
        visual = struct.unpack_from("<I", req, 24)[0]
        mask = struct.unpack_from("<I", req, 28)[0]
        win = {"id": wid, "parent": parent, "x": x, "y": y, "w": max(1, w),
               "h": max(1, h), "depth": depth, "class": wclass, "visual": visual,
               "mapped": False, "event_mask": 0, "cursor": 0,
               "override_redirect": False, "wm_state": set(), "frames": 0,
               "saved_geometry": None}
        self.apply_attributes(win, mask, req[32:])
        self.windows[wid] = win

    def req_change_window_attributes(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        mask = struct.unpack_from("<I", req, 8)[0]
        win = self.windows.get(wid)
        if not win:
            self.send_error(ERR_WINDOW, badvalue=wid, major=2)
            return
        self.apply_attributes(win, mask, req[12:])

    @staticmethod
    def apply_attributes(win, mask, values):
        bit = 0
        off = 0
        while mask:
            if mask & 1:
                v = struct.unpack_from("<I", values, off)[0]
                off += 4
                if bit == 9:
                    win["override_redirect"] = bool(v)
                elif bit == 11:
                    win["event_mask"] = v
                elif bit == 14:
                    win["cursor"] = v
            mask >>= 1
            bit += 1

    def req_get_window_attributes(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.get(wid)
        if not win:
            self.send_error(ERR_WINDOW, badvalue=wid, major=3)
            return
        body = (u32(VISUAL_ID) + u16(win["class"]) + b"\x00"        # bit-gravity
                + struct.pack("B", 0) + u32(0xFFFFFF) + u32(0)        # win-gravity, planes, pixel
                + struct.pack("B", 1) + struct.pack("B", 1 if win["mapped"] else 0)
                + struct.pack("B", 2 if win["mapped"] else 0)         # viewable/unmapped
                + struct.pack("B", 1 if win["override_redirect"] else 0)
                + u32(COLORMAP_ID) + u32(win["event_mask"]) + u32(win["event_mask"])
                + u16(0) + u16(0))
        self.send_reply(1, body[:24], body[24:])

    def req_destroy_window(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.pop(wid, None)
        if win:
            if win["mapped"]:
                self.ev_unmap(win)
            self.ev_destroy(wid)

    def req_destroy_subwindows(self, req):
        parent = struct.unpack_from("<I", req, 4)[0]
        for wid in [w["id"] for w in self.windows.values() if w["parent"] == parent]:
            win = self.windows.pop(wid)
            if win["mapped"]:
                self.ev_unmap(win)
            self.ev_destroy(wid)

    def req_map_window(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.get(wid)
        if not win:
            self.send_error(ERR_WINDOW, badvalue=wid, major=8)
            return
        first_map = not win["mapped"]
        win["mapped"] = True
        self.ev_map(win)
        if win["event_mask"] & EXPOSURE_MASK:
            self.ev_expose(win, 0, 0, win["w"], win["h"])
        if (first_map and win["parent"] == ROOT_ID and self.script == "basic_input"
                and not self.script_started):
            self.script_started = True
            self.schedule_basic_input(win)

    def req_map_subwindows(self, req):
        self.req_map_window(req)

    def req_unmap_window(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.get(wid)
        if win and win["mapped"]:
            win["mapped"] = False
            self.ev_unmap(win)

    def req_configure_window(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        mask = struct.unpack_from("<H", req, 8)[0]
        win = self.windows.get(wid)
        if not win:
            self.send_error(ERR_WINDOW, badvalue=wid, major=12)
            return
        values = req[12:]
        off = 0
        for bit in range(7):
            if mask & (1 << bit):
                v = struct.unpack_from("<i", values, off)[0]
                off += 4
                if bit == 0:
                    win["x"] = v
                elif bit == 1:
                    win["y"] = v
                elif bit == 2:
                    win["w"] = max(1, v)
                elif bit == 3:
                    win["h"] = max(1, v)
        self.ev_configure(win)

    def req_get_geometry(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        if wid == ROOT_ID:
            body = (u32(ROOT_ID) + s16(0) + s16(0) + u16(self.width)
                    + u16(self.height) + u16(0) + b"\0" * 10)
            self.send_reply(24, body)
            return
        win = self.windows.get(wid)
        if not win:
            self.send_error(ERR_DRAWABLE, badvalue=wid, major=14)
            return
        body = (u32(ROOT_ID) + s16(win["x"]) + s16(win["y"]) + u16(win["w"])
                + u16(win["h"]) + u16(0) + b"\0" * 10)
        self.send_reply(win["depth"] or 24, body)

    def req_query_tree(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        children = [w["id"] for w in self.windows.values() if w["parent"] == wid]
        parent = 0 if wid == ROOT_ID else self.windows.get(wid, {}).get("parent", 0)
        body = u32(ROOT_ID) + u32(parent) + u16(len(children)) + b"\0" * 14
        self.send_reply(0, body, b"".join(u32(c) for c in children))

    # -- atoms / properties --------------------------------------------------------

    def req_intern_atom(self, req):
        only_if_exists = req[1]
        name_len = struct.unpack_from("<H", req, 4)[0]
        name = req[8:8 + name_len].decode("latin-1")
        atom = self.atoms.get(name)
        if atom is None and not only_if_exists:
            atom = self.next_atom
            self.next_atom += 1
            self.atoms[name] = atom
            self.atom_names[atom] = name
        self.send_reply(0, u32(atom or 0) + b"\0" * 20)

    def req_get_atom_name(self, req):
        atom = struct.unpack_from("<I", req, 4)[0]
        name = self.atom_names.get(atom)
        if name is None:
            self.send_error(ERR_ATOM, badvalue=atom, major=17)
            return
        data = name.encode("latin-1")
        self.send_reply(0, u16(len(data)) + b"\0" * 22, data)

    def req_change_property(self, req):
        fmt = req[1]
        mode = req[16]
        wid, prop, ptype = struct.unpack_from("<III", req, 4)
        nelem = struct.unpack_from("<I", req, 20)[0]
        nbytes = nelem * (fmt // 8) if fmt else 0
        data = req[24:24 + nbytes]
        key = (wid, prop)
        if mode == 0 or key not in self.properties:      # Replace
            self.properties[key] = (ptype, fmt, data)
        else:
            old_type, old_fmt, old = self.properties[key]
            joined = data + old if mode == 1 else old + data  # Prepend / Append
            self.properties[key] = (old_type, old_fmt, joined)
        win = self.windows.get(wid)
        if win and (win["event_mask"] & PROPERTY_CHANGE_MASK):
            self.ev_property(wid, prop, 0)

    def req_delete_property(self, req):
        wid, prop = struct.unpack_from("<II", req, 4)
        self.properties.pop((wid, prop), None)
        win = self.windows.get(wid)
        if win and (win["event_mask"] & PROPERTY_CHANGE_MASK):
            self.ev_property(wid, prop, 1)

    def req_get_property(self, req):
        delete = req[1]
        wid, prop, _req_type = struct.unpack_from("<III", req, 4)
        long_offset, long_length = struct.unpack_from("<II", req, 16)
        value = self.properties.get((wid, prop))
        if value is None:
            self.send_reply(0, u32(0) + u32(0) + u32(0) + b"\0" * 12)
            return
        ptype, fmt, data = value
        unit = max(1, fmt // 8)
        start = min(long_offset * unit, len(data))
        end = min(start + long_length * unit, len(data))
        chunk = data[start:end]
        bytes_after = len(data) - end
        self.send_reply(fmt, u32(ptype) + u32(bytes_after)
                        + u32(len(chunk) // unit) + b"\0" * 12, chunk)
        if delete and bytes_after == 0:
            self.properties.pop((wid, prop), None)

    # -- client messages / tiny WM --------------------------------------------------

    def req_send_event(self, req):
        dest = struct.unpack_from("<I", req, 4)[0]
        ev = req[12:44]
        if len(ev) < 32 or ev[0] != CLIENT_MESSAGE:
            return
        win_id, msg_type = struct.unpack_from("<II", ev, 4)
        data20 = ev[12:32]
        l = struct.unpack_from("<5i", data20, 0)
        if dest == ROOT_ID and msg_type == self.atoms.get("_NET_WM_STATE"):
            self.wm_net_wm_state(win_id, l[0], l[1], l[2])
        elif dest == ROOT_ID and msg_type == self.atoms.get("WM_CHANGE_STATE"):
            self.wm_iconify(win_id)
        else:
            win = self.windows.get(dest)
            if win:
                self.ev_client_message(win["id"], msg_type, data20)

    def wm_net_wm_state(self, win_id, action, atom1, atom2):
        win = self.windows.get(win_id)
        if not win:
            return
        for atom in (atom1, atom2):
            if not atom:
                continue
            if action == 0:
                win["wm_state"].discard(atom)
            elif action == 1:
                win["wm_state"].add(atom)
            elif action == 2:
                if atom in win["wm_state"]:
                    win["wm_state"].discard(atom)
                else:
                    win["wm_state"].add(atom)
        fs = self.atoms.get("_NET_WM_STATE_FULLSCREEN")
        if fs and fs in win["wm_state"]:
            if win["saved_geometry"] is None:
                win["saved_geometry"] = (win["x"], win["y"], win["w"], win["h"])
            if (win["w"], win["h"]) != (self.width, self.height):
                win["x"], win["y"], win["w"], win["h"] = 0, 0, self.width, self.height
                self.ev_configure(win)
        elif win["saved_geometry"] is not None:
            win["x"], win["y"], win["w"], win["h"] = win["saved_geometry"]
            win["saved_geometry"] = None
            self.ev_configure(win)
        state_prop = self.atoms.get("_NET_WM_STATE")
        fmt_atoms = b"".join(u32(a) for a in sorted(win["wm_state"]))
        self.properties[(win_id, state_prop)] = (PREDEFINED_ATOMS["ATOM"], 32, fmt_atoms)
        self.ev_property(win_id, state_prop, 0)

    def wm_iconify(self, win_id):
        win = self.windows.get(win_id)
        if win and win["mapped"]:
            win["mapped"] = False
            self.ev_unmap(win)

    # -- input / focus ----------------------------------------------------------------

    def req_grab_pointer(self, req):
        self.send_reply(0, b"\0" * 24)  # GrabSuccess

    def req_ungrab_pointer(self, req):
        pass

    def req_grab_keyboard(self, req):
        self.send_reply(0, b"\0" * 24)

    def req_ungrab_keyboard(self, req):
        pass

    def req_query_pointer(self, req):
        wid = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.get(wid)
        rx, ry = self.pointer_root
        wx_, wy_ = (rx - win["x"], ry - win["y"]) if win else (rx, ry)
        body = (u32(ROOT_ID) + u32(wid if win else 0)
                + s16(rx) + s16(ry) + s16(wx_) + s16(wy_) + u16(0) + b"\0" * 6)
        self.send_reply(1, body)

    def req_translate_coordinates(self, req):
        src, dst = struct.unpack_from("<II", req, 4)
        x, y = struct.unpack_from("<hh", req, 12)
        sw = self.windows.get(src, {"x": 0, "y": 0})
        dw = self.windows.get(dst, {"x": 0, "y": 0})
        self.send_reply(1, s16(x + sw["x"] - dw["x"]) + s16(y + sw["y"] - dw["y"])
                        + u32(0) + b"\0" * 16)

    def req_warp_pointer(self, req):
        dst = struct.unpack_from("<I", req, 8)[0]
        dx, dy = struct.unpack_from("<hh", req, 20)
        if dst and dst in self.windows:
            win = self.windows[dst]
            self.pointer_root = (win["x"] + dx, win["y"] + dy)
            if win["event_mask"] & POINTER_MOTION_MASK:
                self.ev_buttonlike(MOTION_NOTIFY, 0, win, dx, dy)

    def req_set_input_focus(self, req):
        focus = struct.unpack_from("<I", req, 4)[0]
        if focus == self.focus_window:
            return
        old = self.focus_window
        self.focus_window = focus
        if old:
            self.ev_focus(FOCUS_OUT, old)
        if focus:
            self.ev_focus(FOCUS_IN, focus)

    def req_get_input_focus(self, req):
        self.send_reply(2, u32(self.focus_window) + b"\0" * 20)  # RevertToParent

    def req_query_keymap(self, req):
        self.send_reply(0, b"\0" * 24, b"\0" * 32)  # 256-bit keymap, all clear

    # -- fonts / cursors / pixmaps / gc -------------------------------------------

    def req_open_font(self, req):
        pass

    def req_close_font(self, req):
        pass

    def req_get_font_path(self, req):
        self.send_reply(0, u16(0) + b"\0" * 22)  # empty font path

    def req_list_fonts(self, req):
        self.send_reply(0, u16(0) + b"\0" * 22)  # no fonts

    def req_create_pixmap(self, req):
        pass

    def req_free_pixmap(self, req):
        pass

    def req_create_gc(self, req):
        pass

    def req_free_gc(self, req):
        pass

    def req_create_colormap(self, req):
        pass

    def req_free_colormap(self, req):
        pass

    def req_create_cursor(self, req):
        pass

    def req_create_glyph_cursor(self, req):
        pass

    def req_free_cursor(self, req):
        pass

    def req_put_image(self, req):
        drawable = struct.unpack_from("<I", req, 4)[0]
        win = self.windows.get(drawable)
        if win:
            win["frames"] += 1
            self.frames_total += 1

    def req_get_image(self, req):
        # tests never read back pixels via X; hand back a 1x1 black image
        self.send_reply(24, u32(VISUAL_ID) + b"\0" * 20, b"\0" * 4)

    # -- keyboard control / extensions --------------------------------------------

    def req_query_extension(self, req):
        name_len = struct.unpack_from("<H", req, 4)[0]
        name = req[8:8 + name_len].decode("latin-1")
        if name == "BIG-REQUESTS":
            self.send_reply(1, struct.pack("BBB", BIG_REQUESTS_OPCODE, 0, 0) + b"\0" * 21)
        else:
            self.send_reply(0, b"\0" * 24)

    def req_list_extensions(self, req):
        name = b"BIG-REQUESTS"
        self.send_reply(1, b"\0" * 24, struct.pack("B", len(name)) + name)

    def req_get_keyboard_mapping(self, req):
        count = req[5]
        self.send_reply(1, b"\0" * 24, b"\0" * (4 * count))

    def req_get_keyboard_control(self, req):
        body = (u32(0) + struct.pack("B", 50) + struct.pack("B", 100)
                + u16(440) + u16(100) + b"\0" * 2 + b"\xff" * 12)
        self.send_reply(1, body, b"\xff" * 20)

    def req_get_modifier_mapping(self, req):
        self.send_reply(0, b"\0" * 24)

    def req_set_modifier_mapping(self, req):
        self.send_reply(0, b"\0" * 24)

    def req_get_screen_saver(self, req):
        body = u16(600) + u16(600) + b"\x01\x01" + b"\0" * 18
        self.send_reply(0, body)

    def req_no_operation(self, req):
        pass

    # -- scripted input -------------------------------------------------------------

    def schedule(self, delay_ms, fn):
        deadline = time.monotonic() + delay_ms / 1000.0
        self.script_events.append((deadline, fn))
        self.script_events.sort(key=lambda e: e[0])

    def schedule_basic_input(self, win):
        wid = win["id"]
        kp = 38  # X11 keycode 38 == evdev keycode 30 == 'a'
        self.schedule(50, lambda: self.ev_focus(FOCUS_IN, wid))
        self.schedule(100, lambda: self.ev_crossing(ENTER_NOTIFY, win, 30, 40))
        self.schedule(150, lambda: self.ev_buttonlike(MOTION_NOTIFY, 0, win, 60, 70))
        self.schedule(200, lambda: self.ev_buttonlike(KEY_PRESS, kp, win, 60, 70, t=1000))

        def repeat_pair():
            # server auto-repeat: release+press with identical timestamps
            self.ev_buttonlike(KEY_RELEASE, kp, win, 60, 70, t=1500)
            self.ev_buttonlike(KEY_PRESS, kp, win, 60, 70, t=1500)
        self.schedule(300, repeat_pair)
        self.schedule(400, lambda: self.ev_buttonlike(KEY_RELEASE, kp, win, 60, 70, t=1900))
        self.schedule(500, lambda: self.ev_buttonlike(BUTTON_PRESS, 1, win, 50, 60))
        self.schedule(560, lambda: self.ev_buttonlike(BUTTON_RELEASE, 1, win, 50, 60))
        self.schedule(620, lambda: self.ev_buttonlike(BUTTON_PRESS, 4, win, 50, 60))
        self.schedule(640, lambda: self.ev_buttonlike(BUTTON_RELEASE, 4, win, 50, 60))

        def close():
            delete = self.atoms.get("WM_DELETE_WINDOW")
            protos = self.atoms.get("WM_PROTOCOLS")
            if delete and protos:
                self.ev_client_message(wid, protos, struct.pack("<5i", delete, 0, 0, 0, 0))
        self.schedule(800, close)

    REQUESTS = {}


MiniXServer.REQUESTS = {
    1: MiniXServer.req_create_window,
    2: MiniXServer.req_change_window_attributes,
    3: MiniXServer.req_get_window_attributes,
    4: MiniXServer.req_destroy_window,
    5: MiniXServer.req_destroy_subwindows,
    8: MiniXServer.req_map_window,
    9: MiniXServer.req_map_subwindows,
    10: MiniXServer.req_unmap_window,
    12: MiniXServer.req_configure_window,
    14: MiniXServer.req_get_geometry,
    15: MiniXServer.req_query_tree,
    16: MiniXServer.req_intern_atom,
    17: MiniXServer.req_get_atom_name,
    18: MiniXServer.req_change_property,
    19: MiniXServer.req_delete_property,
    20: MiniXServer.req_get_property,
    25: MiniXServer.req_send_event,
    26: MiniXServer.req_grab_pointer,
    27: MiniXServer.req_ungrab_pointer,
    31: MiniXServer.req_grab_keyboard,
    32: MiniXServer.req_ungrab_keyboard,
    38: MiniXServer.req_query_pointer,
    40: MiniXServer.req_translate_coordinates,
    41: MiniXServer.req_warp_pointer,
    42: MiniXServer.req_set_input_focus,
    43: MiniXServer.req_get_input_focus,
    44: MiniXServer.req_query_keymap,
    45: MiniXServer.req_open_font,
    46: MiniXServer.req_close_font,
    50: MiniXServer.req_list_fonts,
    52: MiniXServer.req_get_font_path,
    53: MiniXServer.req_create_pixmap,
    54: MiniXServer.req_free_pixmap,
    55: MiniXServer.req_create_gc,
    60: MiniXServer.req_free_gc,
    72: MiniXServer.req_put_image,
    73: MiniXServer.req_get_image,
    78: MiniXServer.req_create_colormap,
    79: MiniXServer.req_free_colormap,
    93: MiniXServer.req_create_cursor,
    94: MiniXServer.req_create_glyph_cursor,
    95: MiniXServer.req_free_cursor,
    98: MiniXServer.req_query_extension,
    99: MiniXServer.req_list_extensions,
    101: MiniXServer.req_get_keyboard_mapping,
    103: MiniXServer.req_get_keyboard_control,
    108: MiniXServer.req_get_screen_saver,
    113: MiniXServer.req_set_modifier_mapping,
    119: MiniXServer.req_get_modifier_mapping,
    127: MiniXServer.req_no_operation,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--display", type=int, default=98)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--script", default="basic_input", choices=["basic_input", "linger"])
    args = ap.parse_args()
    srv = MiniXServer(args.display, args.width, args.height, args.script)
    sys.exit(srv.serve())


if __name__ == "__main__":
    main()
