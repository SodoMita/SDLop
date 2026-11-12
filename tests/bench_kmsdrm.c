/*
  SDLop bench: KMS present-path throughput (software surface path + GL path).

  Measures wall-clock time per present (SDL_UpdateWindowSurface /
  SDL_GL_SwapWindow) so atomic vs legacy modesetting and scanout-path
  changes can be compared. Not a ctest gate; skips cleanly (exit 0) when
  no usable /dev/dri card exists.

  Env:
    BENCH_FRAMES             number of measured frames per path (default 240)
    SDLOP_KMSDRM_DEVICE      usual driver-side device selection
    SDLOP_KMSDRM_ATOMIC=0    force the legacy path for A/B comparison
    SDLOP_TEST_KMSDRM_HOLD   (unset; bench returns the window as soon as
                              the last frame is on screen)

  Output is one line per path, machine-greppable:
    bench_kmsdrm: sw total=...ms avg=...ms p50=...ms min=...ms max=...ms fps=...
    bench_kmsdrm: gl total=...ms avg=...ms p50=...ms min=...ms max=...ms fps=...

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>

static bool kms_present(void)
{
    DIR *folder = opendir("/dev/dri");
    if (!folder) {
        return false;
    }
    bool found = false;
    struct dirent *res;
    while ((res = readdir(folder)) != NULL && !found) {
        if (strncmp(res->d_name, "card", 4) == 0) {
            found = true;
        }
    }
    closedir(folder);
    return found;
}

struct bench_stats {
    double total_ms;
    double avg_ms, p50_ms, min_ms, max_ms;
    double fps;
};

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static void collect(struct bench_stats *stats, double *frame_ms, int frames)
{
    qsort(frame_ms, (size_t)frames, sizeof(double), compare_double);
    stats->min_ms = frame_ms[0];
    stats->max_ms = frame_ms[frames - 1];
    stats->p50_ms = frame_ms[frames / 2];
    double total = 0.0;
    for (int i = 0; i < frames; ++i) {
        total += frame_ms[i];
    }
    stats->total_ms = total;
    stats->avg_ms = total / (double)frames;
    stats->fps = 1000.0 / stats->avg_ms;
}

static void print_stats(const char *tag, const struct bench_stats *st, int frames)
{
    printf("bench_kmsdrm: %s total=%.1fms avg=%.3fms p50=%.3fms min=%.3fms max=%.3fms fps=%.1f (frames=%d)\n",
           tag, st->total_ms, st->avg_ms, st->p50_ms, st->min_ms, st->max_ms, st->fps, frames);
}

static bool bench_sw(int frames, int warmup)
{
    SDL_Window *w = SDL_CreateWindow("SDLop bench sw", 640, 480, 0);
    if (!w) {
        printf("  note: sw window failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (!s) {
        printf("  note: sw surface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(w);
        return false;
    }
    double *frame_ms = (double *)malloc(sizeof(double) * (size_t)(frames + warmup));
    if (!frame_ms) {
        SDL_DestroyWindow(w);
        return false;
    }
    int count = 0;
    for (int i = 0; i < warmup + frames; ++i) {
        /* varying color so each present differs on screen */
        SDL_FillSurfaceRect(s, NULL, 0x000000C0u | ((uint32_t)(i * 37) & 0xFF) << 8 | ((uint32_t)i & 0x7F));
        uint64_t t0 = SDL_GetTicksNS();
        bool ok = SDL_UpdateWindowSurface(w);
        uint64_t t1 = SDL_GetTicksNS();
        SDL_PumpEvents();
        if (!ok) {
            printf("  note: sw present failed at frame %d: %s\n", i, SDL_GetError());
            free(frame_ms);
            SDL_DestroyWindow(w);
            return false;
        }
        if (i >= warmup) {
            frame_ms[count++] = (double)(t1 - t0) / 1e6;
        }
    }
    struct bench_stats st;
    collect(&st, frame_ms, count);
    print_stats("sw", &st, count);
    free(frame_ms);
    SDL_DestroyWindow(w);
    return true;
}

static bool bench_gl(int frames, int warmup)
{
    SDL_Window *w = SDL_CreateWindow("SDLop bench gl", 640, 480, SDL_WINDOW_OPENGL);
    if (!w) {
        printf("  note: GL window failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) {
        printf("  note: GL context failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(w);
        return false;
    }
    typedef void (*PFN_glClearColor)(float, float, float, float);
    typedef void (*PFN_glClear)(unsigned int);
    PFN_glClearColor pClearColor = (PFN_glClearColor)SDL_GL_GetProcAddress("glClearColor");
    PFN_glClear pClear = (PFN_glClear)SDL_GL_GetProcAddress("glClear");
    if (!pClearColor || !pClear) {
        printf("  note: GL entry points missing\n");
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(w);
        return false;
    }
    SDL_GL_SetSwapInterval(1); /* vsync; compares equally across runs */

    double *frame_ms = (double *)malloc(sizeof(double) * (size_t)(frames + warmup));
    if (!frame_ms) {
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(w);
        return false;
    }
    int count = 0;
    for (int i = 0; i < warmup + frames; ++i) {
        float phase = (float)((i * 37) & 255) / 255.0f;
        pClearColor(0.9f * phase, 0.1f, 0.5f, 1.0f);
        pClear(0x00004000u); /* GL_COLOR_BUFFER_BIT */
        uint64_t t0 = SDL_GetTicksNS();
        SDL_GL_SwapWindow(w);
        uint64_t t1 = SDL_GetTicksNS();
        SDL_PumpEvents();
        if (i >= warmup) {
            frame_ms[count++] = (double)(t1 - t0) / 1e6;
        }
    }
    struct bench_stats st;
    collect(&st, frame_ms, count);
    print_stats("gl", &st, count);
    free(frame_ms);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(w);
    return true;
}

int main(void)
{
    int failures = 0;

    if (!kms_present()) {
        printf("bench_kmsdrm: SKIP (no /dev/dri/cardN)\n");
        return 0;
    }
    setenv("SDL_VIDEODRIVER", "kmsdrm", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    int frames = 240;
    const char *fstr = getenv("BENCH_FRAMES");
    if (fstr && atoi(fstr) > 0 && atoi(fstr) <= 10000) {
        frames = atoi(fstr);
    }
    const int warmup = 20;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("bench_kmsdrm: SKIP (kmsdrm unavailable: %s)\n", SDL_GetError());
        return 0;
    }
    printf("bench_kmsdrm: driver=%s atomic=%s frames=%d\n",
           SDL_GetCurrentVideoDriver(),
           getenv("SDLOP_KMSDRM_ATOMIC") ? getenv("SDLOP_KMSDRM_ATOMIC") : "default(on)",
           frames);

    if (!bench_sw(frames, warmup)) {
        failures++;
    }
    if (!bench_gl(frames, warmup)) {
        /* GL failure is not fatal for the bench */
        printf("  note: GL path not benchmarked\n");
    }

    SDL_Quit();
    printf("bench_kmsdrm: done%s\n", failures ? " (sw path FAILED)" : "");
    return failures ? 1 : 0;
}
