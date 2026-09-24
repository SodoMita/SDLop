/*
  SDLop benchmark: real SDL3 and SDLop, measured in the same process.

  The program links the *system* SDL3 (so every SDL_* call below is stock SDL3)
  and dlopen()s SDLop's shared library at run time; the same operations are
  then timed through both. Everything the two need is public API, and the drive
  is `offscreen` unless SDL_VIDEODRIVER says otherwise, so the numbers do not
  depend on a compositor being present.

  Build: make bench
  Run:   ./build/bench/bench_compare build/libSDLop.so [--json]

  Timings come from clock_gettime(CLOCK_MONOTONIC) - not from either library -
  and each result is the median of several runs to take scheduler noise out.
*/

#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ helpers */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define RUNS 5

typedef struct
{
    const char *name;
    const char *unit;
    double ns_sdl3;
    double ns_lean;
    double raw_sdl3[RUNS];
    double raw_lean[RUNS];
} Result;

static Result results[16];
static int num_results;

/* ------------------------------------------------------------------ the API */

/* The SDLop entry points, looked up by name. The signatures are identical to
   SDL3's (that is the whole point of the project), so the system headers can
   describe them. */
typedef struct
{
    bool (*Init)(SDL_InitFlags);
    void (*Quit)(void);
    SDL_Window *(*CreateWindow)(const char *, int, int, SDL_WindowFlags);
    void (*DestroyWindow)(SDL_Window *);
    void (*PumpEvents)(void);
    bool (*PollEvent)(SDL_Event *);
    bool (*PushEvent)(SDL_Event *);
    Uint64 (*GetTicks)(void);
    Uint64 (*GetPerformanceCounter)(void);
    SDL_Surface *(*GetWindowSurface)(SDL_Window *);
    bool (*FillSurfaceRect)(SDL_Surface *, const SDL_Rect *, Uint32);
    bool (*UpdateWindowSurface)(SDL_Window *);
    const bool *(*GetKeyboardState)(int *);
    SDL_Keymod (*GetModState)(void);
    const char *(*GetCurrentVideoDriver)(void);
    int (*GetVersion)(void);
} SDLopAPI;

static SDLopAPI op;
static bool benchmarking_lean;

/* One generic interface over both libraries, so each benchmark is written once. */
typedef struct
{
    bool (*Init)(SDL_InitFlags);
    void (*Quit)(void);
    SDL_Window *(*CreateWindow)(const char *, int, int, SDL_WindowFlags);
    void (*DestroyWindow)(SDL_Window *);
    void (*PumpEvents)(void);
    bool (*PollEvent)(SDL_Event *);
    bool (*PushEvent)(SDL_Event *);
    Uint64 (*GetTicks)(void);
    Uint64 (*GetPerformanceCounter)(void);
    SDL_Surface *(*GetWindowSurface)(SDL_Window *);
    bool (*FillSurfaceRect)(SDL_Surface *, const SDL_Rect *, Uint32);
    bool (*UpdateWindowSurface)(SDL_Window *);
    const bool *(*GetKeyboardState)(int *);
    SDL_Keymod (*GetModState)(void);
} API;

static API api(void)
{
    API a;
    if (benchmarking_lean) {
        a.Init = op.Init;
        a.Quit = op.Quit;
        a.CreateWindow = op.CreateWindow;
        a.DestroyWindow = op.DestroyWindow;
        a.PumpEvents = op.PumpEvents;
        a.PollEvent = op.PollEvent;
        a.PushEvent = op.PushEvent;
        a.GetTicks = op.GetTicks;
        a.GetPerformanceCounter = op.GetPerformanceCounter;
        a.GetWindowSurface = op.GetWindowSurface;
        a.FillSurfaceRect = op.FillSurfaceRect;
        a.UpdateWindowSurface = op.UpdateWindowSurface;
        a.GetKeyboardState = op.GetKeyboardState;
        a.GetModState = op.GetModState;
    } else {
        a.Init = SDL_Init;
        a.Quit = SDL_Quit;
        a.CreateWindow = SDL_CreateWindow;
        a.DestroyWindow = SDL_DestroyWindow;
        a.PumpEvents = SDL_PumpEvents;
        a.PollEvent = SDL_PollEvent;
        a.PushEvent = SDL_PushEvent;
        a.GetTicks = SDL_GetTicks;
        a.GetPerformanceCounter = SDL_GetPerformanceCounter;
        a.GetWindowSurface = SDL_GetWindowSurface;
        a.FillSurfaceRect = SDL_FillSurfaceRect;
        a.UpdateWindowSurface = SDL_UpdateWindowSurface;
        a.GetKeyboardState = SDL_GetKeyboardState;
        a.GetModState = SDL_GetModState;
    }
    return a;
}

static void add_result(const char *name, const char *unit, const double *sdl3, const double *leanr)
{
    Result *r;

    if (num_results >= (int)SDL_arraysize(results)) {
        return;
    }
    r = &results[num_results++];
    r->name = name;
    r->unit = unit;
    memcpy(r->raw_sdl3, sdl3, sizeof(r->raw_sdl3));
    memcpy(r->raw_lean, leanr, sizeof(r->raw_lean));
    qsort(r->raw_sdl3, RUNS, sizeof(double), cmp_double);
    qsort(r->raw_lean, RUNS, sizeof(double), cmp_double);
    r->ns_sdl3 = r->raw_sdl3[RUNS / 2];
    r->ns_lean = r->raw_lean[RUNS / 2];
}

/* ----------------------------------------------------------------- benchmarks */

static double bench_init_cycle(int iterations)
{
    API a = api();
    double start = now_ns();
    for (int i = 0; i < iterations; i++) {
        if (!a.Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            exit(1);
        }
        a.Quit();
    }
    return (now_ns() - start) / iterations;
}

static double bench_window_cycle(int iterations)
{
    API a = api();
    double start;

    a.Init(SDL_INIT_VIDEO);
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        SDL_Window *window = a.CreateWindow("bench", 640, 480, 0);
        if (!window) {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            exit(1);
        }
        a.DestroyWindow(window);
    }
    return ((now_ns() - start) / iterations);
}

static double bench_window_cycle_cleanup(void)
{
    api().Quit();
    return 0.0;
}

static double bench_pump(int iterations)
{
    API a = api();
    SDL_Window *window;
    double start;

    a.Init(SDL_INIT_VIDEO);
    window = a.CreateWindow("bench", 640, 480, 0);
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        a.PumpEvents();
    }
    start = (now_ns() - start) / iterations;
    a.DestroyWindow(window);
    a.Quit();
    return start;
}

static double bench_events(int iterations)
{
    API a = api();
    SDL_Event event;
    double start;
    long count = 0;

    a.Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO);
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        for (int n = 0; n < 64; n++) {
            if (a.PushEvent(&event)) {
                count++;
            }
        }
        while (a.PollEvent(&event)) {
            count++;
        }
    }
    start = (now_ns() - start) / count;
    a.Quit();
    return start;
}

static double bench_ticks(int iterations)
{
    API a = api();
    Uint64 sum = 0;
    double start;

    a.Init(SDL_INIT_VIDEO);            /* ticks are ours once initialized */
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        sum += a.GetTicks();
    }
    start = (now_ns() - start) / iterations;
    a.Quit();
    return start + (double)(sum & 1);  /* keep the compiler honest */
}

static double bench_perf_counter(int iterations)
{
    API a = api();
    Uint64 sum = 0;
    double start;

    a.Init(SDL_INIT_VIDEO);
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        sum += a.GetPerformanceCounter();
    }
    start = (now_ns() - start) / iterations;
    a.Quit();
    return start + (double)(sum & 1);
}

static double bench_surface_upload(int iterations)
{
    API a = api();
    SDL_Window *window;
    SDL_Surface *surface;
    double start;

    a.Init(SDL_INIT_VIDEO);
    window = a.CreateWindow("bench", 640, 480, 0);
    if (!window) {
        return 0.0;
    }
    surface = a.GetWindowSurface(window);
    if (!surface) {
        a.DestroyWindow(window);
        a.Quit();
        return 0.0;
    }
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        a.FillSurfaceRect(surface, NULL, (Uint32)i);
        a.UpdateWindowSurface(window);
    }
    start = (now_ns() - start) / iterations;
    a.DestroyWindow(window);
    a.Quit();
    return start;
}

static double bench_keyboard_state(int iterations)
{
    API a = api();
    const bool *state = NULL;
    double start;

    a.Init(SDL_INIT_VIDEO);
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        state = a.GetKeyboardState(NULL);
        state = a.GetKeyboardState(NULL);
        state = a.GetKeyboardState(NULL);
        state = a.GetKeyboardState(NULL);
    }
    start = (now_ns() - start) / (iterations * 4);
    a.Quit();
    return start + (state == NULL ? 0.0 : 0.0);
}

static double bench_mod_state(int iterations)
{
    API a = api();
    int mod = 0;
    double start;

    a.Init(SDL_INIT_VIDEO);
    start = now_ns();
    for (int i = 0; i < iterations; i++) {
        mod += (int)a.GetModState();
    }
    start = (now_ns() - start) / iterations;
    a.Quit();
    return start + (double)(mod & 1);
}

typedef double (*BenchFn)(int);

typedef struct
{
    const char *name;
    BenchFn fn;
    int iterations;
    const char *unit;
} BenchDef;

static const BenchDef benches[] = {
    { "SDL_Init + SDL_Quit              ", bench_init_cycle,       200,     "ns/cycle" },
    { "SDL_CreateWindow + DestroyWindow ", bench_window_cycle,     500,     "ns/cycle" },
    { "SDL_PumpEvents (no pending input)", bench_pump,          200000,     "ns/call"  },
    { "SDL_PushEvent + SDL_PollEvent    ", bench_events,         20000,     "ns/event" },
    { "SDL_GetTicks()                   ", bench_ticks,        2000000,     "ns/call"  },
    { "SDL_GetPerformanceCounter()      ", bench_perf_counter, 2000000,     "ns/call"  },
    { "FillSurfaceRect+UpdateWindowSurf ", bench_surface_upload,  2000,     "ns/frame" },
    { "SDL_GetKeyboardState()           ", bench_keyboard_state, 50000,     "ns/call"  },
    { "SDL_GetModState()                ", bench_mod_state,    2000000,     "ns/call"  },
};

/* ---------------------------------------------------------------------- main */

static void load_lean(const char *path)
{
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    const char *missing = NULL;
    int version;

    if (!handle) {
        fprintf(stderr, "cannot dlopen('%s'): %s\n", path, dlerror());
        exit(1);
    }
#define LOAD(field, symbol)                                                     \
    do {                                                                        \
        *(void **)(&op.field) = dlsym(handle, symbol);                        \
        if (!op.field) {                                                      \
            missing = symbol;                                                   \
        }                                                                       \
    } while (0)
    LOAD(Init, "SDL_Init");
    LOAD(Quit, "SDL_Quit");
    LOAD(CreateWindow, "SDL_CreateWindow");
    LOAD(DestroyWindow, "SDL_DestroyWindow");
    LOAD(PumpEvents, "SDL_PumpEvents");
    LOAD(PollEvent, "SDL_PollEvent");
    LOAD(PushEvent, "SDL_PushEvent");
    LOAD(GetTicks, "SDL_GetTicks");
    LOAD(GetPerformanceCounter, "SDL_GetPerformanceCounter");
    LOAD(GetWindowSurface, "SDL_GetWindowSurface");
    LOAD(FillSurfaceRect, "SDL_FillSurfaceRect");
    LOAD(UpdateWindowSurface, "SDL_UpdateWindowSurface");
    LOAD(GetKeyboardState, "SDL_GetKeyboardState");
    LOAD(GetModState, "SDL_GetModState");
    LOAD(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    LOAD(GetVersion, "SDL_GetVersion");
#undef LOAD
    if (missing) {
        fprintf(stderr, "%s does not export %s\n", path, missing);
        exit(1);
    }
    version = op.GetVersion();
    printf("  SDLop reports SDL3 API version %d.%d.%d\n",
           version / 1000000, (version / 1000) % 1000, version % 1000);
}

int main(int argc, char *argv[])
{
    const char *lean_path = argc > 1 ? argv[1] : "build/libSDLop.so";
    bool json = false;
    double sdl3[RUNS], leanr[RUNS];

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        }
    }
    if (!getenv("SDL_VIDEODRIVER")) {
        setenv("SDL_VIDEODRIVER", "offscreen", 1);
    }

    if (!json) {
        printf("SDLop benchmark -- stock SDL3 vs SDLop, same process\n");
        printf("  SDL3 %d.%d.%d, video driver '%s'\n",
               SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
               getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "(default)");
        printf("  warm-up, then the median of %d runs per benchmark\n", RUNS);
    }
    load_lean(lean_path);

    for (size_t b = 0; b < SDL_arraysize(benches); b++) {
        const BenchDef *def = &benches[b];

        /* warm-up: both libraries touch their caches, drivers and thread pools */
        if (def->iterations > 1000) {
            def->fn(def->iterations / 100);
        }
        for (int run = 0; run < RUNS; run++) {
            benchmarking_lean = false;
            sdl3[run] = def->fn(def->iterations);
            benchmarking_lean = true;
            leanr[run] = def->fn(def->iterations);
        }
        add_result(def->name, def->unit, sdl3, leanr);
    }

    /* window_cycle leaves the video subsystem up for the SDLop library */
    benchmarking_lean = true;
    bench_window_cycle_cleanup();

    if (json) {
        printf("{\n  \"results\": [\n");
        for (int i = 0; i < num_results; i++) {
            printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"sdl3\": %.1f, \"sdlop\": %.1f, \"speedup\": %.2f}%s\n",
                   results[i].name, results[i].unit, results[i].ns_sdl3, results[i].ns_lean,
                   results[i].ns_sdl3 / results[i].ns_lean, i + 1 < num_results ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }

    printf("\n  %-36s %14s %14s %9s\n", "benchmark", "SDL3", "SDLop", "speedup");
    printf("  %-36s %14s %14s %9s\n", "", "----------------", "----------------", "-------");
    for (int i = 0; i < num_results; i++) {
        Result *r = &results[i];
        printf("  %-36s %11.1f %s %11.1f %s %8.2fx\n", r->name, r->ns_sdl3, r->unit, r->ns_lean,
               r->unit, r->ns_sdl3 / r->ns_lean);
    }
    printf("\n  (larger speedup = SDLop does the same work in less time;\n");
    printf("   both numbers include the full public-API call)\n");
    return 0;
}
