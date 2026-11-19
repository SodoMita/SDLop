#!/usr/bin/env python3
"""tools/api_add.py FILE.c... : add every exported SDL_* function defined in
the given sources to tools/sdlop_api.txt (keeps the list sorted per run)."""
import re, sys, os
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdlop_api.txt")
lines = open(p).read().splitlines()
have = {l.strip() for l in lines if l.strip() and not l.startswith("#")}
new = set()
for f in sys.argv[1:]:
    src = open(f).read()
    for m in re.finditer(r"^(?!static)[A-Za-z_][\w \*]*?\b(SDL_\w+)\s*\([^;{]*\)\s*\{", src, re.M):
        new.add(m.group(1))
add = sorted(new - have)
with open(p, "a") as fh:
    for n in add:
        fh.write(n + "\n")
print("added %d" % len(add))
