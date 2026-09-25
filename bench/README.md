# bench — stock SDL3 vs SDLop

One program, one process, both libraries, the same operations timed twice.

```sh
make bench
./build/bench/bench_compare build/libSDLop.so
./build/bench/bench_compare build/libSDLop.so --json
make bench-run        # `make bench` + the run above
```

## How it stays honest

* `bench.c` is compiled against the **system** SDL3 headers and linked against
  `libSDL3.so`, so every direct `SDL_*` call in it is stock SDL3.
* SDLop is loaded with `dlopen(RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND)` and its
  entry points are resolved with `dlsym`. `RTLD_DEEPBIND` matters: it makes the
  two libraries resolve their own internal symbols, so neither ends up running
  the other's code and the state of one cannot leak into the other.
  (`RTLD_LOCAL` alone is *not* enough — the executable's already-loaded `libSDL3`
  would win the symbol lookups and the process would crash on the first call.)
* Each benchmark runs through a table of function pointers, so the measurement
  loop is the same source for both implementations.
* 5 runs each, median reported; a warm-up pass runs first; timings are taken with
  `clock_gettime(CLOCK_MONOTONIC)` in the benchmark itself.
* `SDL_VIDEODRIVER=offscreen` by default, so the numbers do not depend on a
  compositor. Cross-check with `SDL_VIDEODRIVER=wayland` in a session if you want
  to know how much window creation costs there.

## Requirements

* Real SDL3 development files (`pkg-config --exists sdl3`, Debian:
  `libsdl3-dev`).
* `build/libSDLop.so` — i.e. run `make` first.

## Interpreting the numbers

`ratio = SDL3 time / SDLop time`, so **> 1 means SDLop is faster**. A ratio
near 1 is a tie and is reported as such (the clock reads in `SDL_GetTicks` and
`SDL_GetPerformanceCounter` are the same `clock_gettime` call in both).

The `FillSurfaceRect + UpdateWindowSurface` line compares "push a frame to an
offscreen window", not present cost on a real compositor: SDL3's offscreen driver
sends the frame through its renderer-side path while SDLop presents the
surface buffer directly. Treat it as a measure of how much machinery sits between
the application and the pixels, not as Wayland present performance — run it with
`SDL_VIDEODRIVER=x11 DISPLAY=:99` (or on a real Wayland session) to see the
present cost both libraries actually pay.

See [../docs/PERFORMANCE.md](../docs/PERFORMANCE.md) for the current results, the
method, and the three hot paths that this benchmark found were slower than SDL3
before they were fixed.
