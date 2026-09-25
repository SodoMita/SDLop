#!/usr/bin/env python3
"""
check_api.py -- prove that SDLop's headers are a faithful subset of SDL3's.

For every header SDLop ships, this compares the declarations clang sees in

    include/SDL3/<header>              (SDLop)
    /usr/include/SDL3/<header>         (upstream SDL3)

and reports three things:

  MISSING   upstream declares it, SDLop does not, and tools/dropped.txt does
            not list it -> this is a bug (or the drop list is out of date)
  EXTRA     SDLop declares it, upstream does not -> only allowed for the
            documented SDL_SDLOP extensions
  CHANGED   same name, different signature -> always a bug

Function declarations that SDLop ships must also actually exist in the built
library; pass --lib <path to libSDLop.so> to check that too.

Usage:
    tools/check_api.py [--upstream /usr/include] [--lib build/libSDLop.so]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Headers that are SDLop's own (not extracted from upstream) and the
# documented extensions that make them differ on purpose.
# Hand-written headers: their surface is a documented subset of upstream's, so a
# delta is reported as information rather than as a failure.
OWN_HEADERS = {"SDL_stdinc.h", "SDL_begin_code.h", "SDL_close_code.h",
               "SDL.h", "SDL_main.h"}
# Names SDLop declares in one header that upstream declares in a different one.
# SDL_touch.h is not part of the core-only surface (touch is not implemented),
# but the SDL_EVENT_FINGER_* events are, so the two ID typedefs the touch event
# struct needs live next to it in SDL_events.h. Their layout is verified by
# tools/abi_probe.c.
RELOCATED = {
    "SDL_events.h": {"SDL_TouchID", "SDL_FingerID"},
}

EXTENSION_NAMES = {"SDL_GetSDLopVersion", "SDL_SDLOP", "SDL_SDLOP_MAJOR_VERSION",
                   "SDL_SDLOP_MINOR_VERSION", "SDL_SDLOP_MICRO_VERSION",
                   "SDL_SDLopVersion", "SDL_SDLOP_IMPLEMENTED_SUBSYSTEMS",
                   "SDL_sdlop_malloc", "SDL_sdlop_calloc", "SDL_sdlop_realloc",
                   "SDL_sdlop_free", "SDL_sdlop_assert_data"}

DECL_RX = re.compile(
    r"^(?P<kind>FunctionDecl|TypedefDecl|RecordDecl|EnumDecl|VarDecl|EnumConstantDecl)\b"
    r".*?<(?P<file>[^<>]*?):(?P<line>\d+):\d+"
    r".*?(?:line:\d+:\d+)?>"
    r"(?P<rest>.*)$")

TYPE_STRIP = [
    (re.compile(r"__attribute__\(\(.*?\)\)"), ""),
    (re.compile(r"\bSDL_DECLSPEC\b"), ""),
    (re.compile(r"\bSDLCALL\b"), ""),
    (re.compile(r"\bSDL_NODISCARD\b|\bSDL_MALLOC\b|\bSDL_ALLOC_SIZE2?\([^)]*\)"), ""),
    (re.compile(r"\bSDL_PRINTF_VARARG_FUNC[V]?\([^)]*\)"), ""),
    (re.compile(r"\bSDL_SCANF_VARARG_FUNC[V]?\([^)]*\)"), ""),
    (re.compile(r"\bSDL_OUT[A-Z_]*CAP\([^)]*\)|\bSDL_IN[A-Z_]*CAP\([^)]*\)"), ""),
    (re.compile(r"\b(SDL_[A-Z_]*FORMAT_STRING)\b"), ""),
    (re.compile(r"\s+"), " "),
]


def norm(type_text):
    t = type_text
    for rx, rep in TYPE_STRIP:
        t = rx.sub(rep, t)
    return t.strip()


def dump_decls(header, include_dir, kind_filter=True):
    """Return {(kind, name): signature} for declarations defined in `header`."""
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as fh:
        fh.write("#include <SDL3/%s>\n" % header[1])
        path = fh.name
    cmd = ["clang", "-fsyntax-only", "-std=c11", "-I", include_dir,
           "-Xclang", "-ast-dump", "-Xclang", "-ast-dump-filter=SDL", path]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
    finally:
        os.unlink(path)
    if res.returncode != 0:
        raise SystemExit("clang failed on %s:\n%s" % (header[1], res.stderr[:2000]))
    out = res.stdout
    decls = {}
    for line in out.split("\n"):
        m = DECL_RX.match(line.strip())
        if not m:
            continue
        kind = m.group("kind")
        if not kind_filter and kind != "FunctionDecl":
            continue
        f = m.group("file")
        if "SDL3/%s" % header[1] not in f.replace("\\", "/"):
            continue
        rest = m.group("rest")
        if kind == "RecordDecl":
            mm = re.search(r"\b(struct|union|enum)\s+([A-Za-z_]\w*)", rest)
            if not mm or "{" in rest.split("'")[0]:
                continue
            decls[(kind, mm.group(2))] = " ".join(mm.groups())
        elif kind == "EnumDecl":
            mm = re.search(r"\benum\s+([A-Za-z_]\w*)", rest)
            if mm:
                decls[(kind, mm.group(1))] = "enum " + mm.group(1)
        elif kind == "EnumConstantDecl":
            mm = re.match(r"\s*([A-Za-z_]\w*)\b", rest.replace("used", "", 1))
            if mm:
                decls[(kind, mm.group(1))] = ""
        else:
            # "... col:42 SDL_CreateWindow 'SDL_Window *(const char *, int, int,
            #  SDL_WindowFlags)' extern" - the name is the word before the last
            # quoted signature; "extern"/"static inline"/attributes may follow it.
            matches = re.findall(r"\b([A-Za-z_]\w*)\s+'([^']*)'", rest)
            if not matches:
                continue
            name, sig = matches[-1]
            decls[(kind, name)] = norm(sig)
    return decls


def dump_macros(header, include_dir):
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as fh:
        fh.write("#include <SDL3/%s>\n" % header[1])
        path = fh.name
    out = subprocess.run(["clang", "-E", "-dM", "-I", include_dir, path],
                         capture_output=True, text=True).stdout
    os.unlink(path)
    macros = {}
    for line in out.split("\n"):
        m = re.match(r"#define\s+([A-Za-z_]\w*)", line)
        if m and m.group(1).startswith("SDL"):
            macros[m.group(1)] = line.strip()
    return macros


def load_dropped():
    """Return {header: {name, ...}} of everything SDLop intentionally omits."""
    dropped = {}
    path = os.path.join(ROOT, "tools", "dropped.txt")
    if not os.path.exists(path):
        return dropped
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        hdr, kind, name = parts[0], parts[1], parts[2]
        name = name.split()[-1].strip("*()") if " " in name else name
        dropped.setdefault(hdr, set()).add(name)
    return dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", default="/usr/include")
    ap.add_argument("--include", default=os.path.join(ROOT, "include"))
    ap.add_argument("--lib", default=None)
    args = ap.parse_args()

    dropped = load_dropped()
    inc = os.path.abspath(args.include)
    up_inc = os.path.abspath(args.upstream)
    headers = sorted(os.listdir(os.path.join(inc, "SDL3")))
    headers = [h for h in headers if h.endswith(".h")]

    problems = 0
    checked = 0
    for h in headers:
        if h in ("SDL.h", "SDL_begin_code.h", "SDL_close_code.h"):
            print("%-22s (glue header, not compared)" % h)
            continue
        own = h in OWN_HEADERS
        if not os.path.exists(os.path.join(up_inc, "SDL3", h)):
            print("%-22s (SDLop-only header, not compared)" % h)
            continue
        decls = dump_decls((inc, h), inc) if not own else {}
        up = dump_decls((up_inc, h), up_inc)
        macros = dump_macros((inc, h), inc) if own else {}
        up_macros = dump_macros((up_inc, h), up_inc) if own else {}
        checked += 1

        allow = dropped.get(h, set())
        info = own
        missing, changed = [], []
        for key, sig in up.items():
            kind, name = key
            if key in decls:
                if kind in ("FunctionDecl", "TypedefDecl", "VarDecl") and decls[key] != sig:
                    changed.append((name, sig, decls[key]))
            elif name in allow or name in EXTENSION_NAMES:
                pass
            else:
                missing.append("%s %s" % (kind, name))
        relocated = RELOCATED.get(h, ())
        extra = [n for (k, n) in decls if (k, n) not in up and
                 n not in EXTENSION_NAMES and n not in relocated and not own]

        missing_macros = []
        for name in up_macros:
            if name not in macros:
                if name in allow or name in EXTENSION_NAMES or name.startswith("SDL_SDLOP"):
                    continue
                if name in ("SDL_stdinc_h_", "SDL_begin_code_h_", "SDL_close_code_h_",
                            "SDL_DECLSPEC", "SDLCALL"):
                    continue
                missing_macros.append(name)

        status = []
        if missing:
            status.append("%d %s" % (len(missing), "upstream-only" if info else "missing"))
        if extra:
            status.append("%d extra" % len(extra))
        if changed:
            status.append("%d changed" % len(changed))
        if missing_macros:
            status.append("%d macros upstream-only" % len(missing_macros))
        print("%-22s %s" % (h, ", ".join(status) if status else "identical surface"))
        for m in sorted(missing):
            print("    %s  %s" % ("NOTE    " if info else "MISSING ", m))
        for e in sorted(extra):
            print("    EXTRA    %s" % e)
        for name, want, got in changed:
            print("    CHANGED  %s\n      upstream: %s\n      sdlop: %s" % (name, want, got))
        for m in sorted(missing_macros):
            print("    %s  %s" % ("NOTE    " if info else "MACRO   ", m))
        if not info:
            problems += len(missing) + len(extra) + len(changed) + len(missing_macros)

    if args.lib:
        # every function the SDLop headers declare must be *defined* by the library,
        # otherwise a program that compiles against them fails to link.
        funcs = set()
        for h in headers:
            if h in OWN_HEADERS:
                continue
            for (kind, name) in dump_decls((inc, h), inc):
                if kind == "FunctionDecl":
                    funcs.add(name)
        nm = subprocess.run(["nm", "-D", "--defined-only", args.lib],
                            capture_output=True, text=True)
        exported = set()
        for line in nm.stdout.split("\n"):
            parts = line.split()
            if len(parts) >= 3 and parts[1] in ("T", "W", "B", "D"):
                exported.add(parts[2])
        if nm.returncode != 0 or not exported:
            print("\nwarning: could not read symbols from %s" % args.lib)
        else:
            undeclared_impl = sorted(f for f in funcs if f not in exported)
            print("\n%d functions declared in the SDLop headers, %d SDL_* symbols exported by %s"
                  % (len(funcs), len([e for e in exported if e.startswith("SDL_")]),
                     os.path.basename(args.lib)))
            if undeclared_impl:
                print("declared but not exported by %s:" % args.lib)
                for f in undeclared_impl:
                    print("    %s" % f)
                problems += len(undeclared_impl)
            else:
                print("all of them are exported by %s" % args.lib)

    print("\nchecked %d headers against upstream SDL3; %s"
          % (checked, "OK" if problems == 0 else "%d problems" % problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
