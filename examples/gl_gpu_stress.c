/*
  SDLop example: GPU stress scene — many spinning, blended triangles over a
  procedural background, intended to push fragment throughput so GPU
  utilization differences between display paths are visible.

  Usage:
    gl_gpu_stress [--duration SECONDS] [--frames N] [--triangles N]
                  [--size WxH] [--vsync | --no-vsync]

  Defaults: 12 s, 1500 triangles, 1280x720, no vsync.
*/

#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Background: per-pixel procedural plasma (fixed 24-step loop). */
static const char *bg_vs =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *bg_fs =
    "precision mediump float;\n"
    "uniform vec2 uRes;\n"
    "uniform float uTime;\n"
    "void main() {\n"
    "  vec2 p = (gl_FragCoord.xy - 0.5 * uRes) / max(uRes.y, 1.0);\n"
    "  float v = 0.0;\n"
    "  vec2 q = p * 2.0;\n"
    "  for (int i = 0; i < 64; i++) {\n"
    "    q = vec2(q.x + sin(q.y * 3.0 + uTime * 0.7), q.y + cos(q.x * 3.0 - uTime * 0.5)) * 0.94;\n"
    "    v += length(q) * 0.01;\n"
    "  }\n"
    "  float w = sin(v * 12.0 + uTime);\n"
    "  gl_FragColor = vec4(0.05 + 0.10 * w, 0.06 + 0.18 * sin(v * 7.0), 0.12 + 0.22 * cos(v * 9.0), 1.0);\n"
    "}\n";

/* Triangle shader: rotation + a few sin/cos per fragment for extra work. */
static const char *tri_vs =
    "attribute vec2 aPos;\n"
    "attribute vec3 aColor;\n"
    "uniform vec2 uCenter;\n"
    "uniform float uAngle;\n"
    "uniform float uScale;\n"
    "uniform vec2 uRes;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  float c = cos(uAngle), s = sin(uAngle);\n"
    "  vec2 p = vec2(aPos.x * c - aPos.y * s, aPos.x * s + aPos.y * c) * uScale;\n"
    "  p += uCenter;\n"
    "  vec2 ndc = vec2(p.x / max(uRes.x / uRes.y, 1.0), p.y);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  vColor = aColor;\n"
    "}\n";

static const char *tri_fs =
    "precision mediump float;\n"
    "varying vec3 vColor;\n"
    "uniform float uTime;\n"
    "void main() {\n"
    "  /* Deliberately expensive per-fragment work. */\n"
    "  vec2 q = gl_FragCoord.xy * 0.01;\n"
    "  float f = 0.0;\n"
    "  for (int i = 0; i < 12; i++) {\n"
    "    q = vec2(q.x + sin(q.y * 1.7 + uTime), q.y + cos(q.x * 1.3 - uTime));\n"
    "    f += length(q) * 0.02;\n"
    "  }\n"
    "  f = 0.55 + 0.45 * sin(f * 3.0);\n"
    "  gl_FragColor = vec4(vColor * f, 0.55);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile error: %s\n", log);
    }
    return sh;
}

static GLuint build_program(const char *vs_src, const char *fs_src,
                            const char *pos_attr, const char *color_attr)
{
    GLuint prog = glCreateProgram();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, pos_attr);
    if (color_attr) {
        glBindAttribLocation(prog, 1, color_attr);
    }
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--duration SECONDS] [--frames N] [--triangles N]\n"
            "          [--size WxH] [--vsync | --no-vsync]\n"
            "  --triangles N    Number of blended triangles per frame (default 1500)\n"
            "  --size WxH       Window size (default 1280x720)\n"
            "  --scale F        Triangle size multiplier (default 1.0)\n"
            "  --offload        Force GL offload via SDL_SetHint(SDLOP_HINT_GL_OFFLOAD)\n"
            "  --read-rgba/rgb/bgra  Override offload readback format (default: driver preferred)\n",
            argv0);
}

typedef struct {
    float center[2];
    float color[3];
    float scale;
    float spin;
} Tri;

int main(int argc, char *argv[])
{
    double duration_sec = 12.0;
    int bench_frames = 0;
    int triangle_count = 1500;
    int win_w = 1280, win_h = 720;
    float scale_mult = 1.0f;
    int swap_interval = 0;
    bool force_offload = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if ((!strcmp(a, "--duration") || !strcmp(a, "-d")) && i + 1 < argc) {
            duration_sec = atof(argv[++i]);
        } else if ((!strcmp(a, "--frames") || !strcmp(a, "-f")) && i + 1 < argc) {
            bench_frames = atoi(argv[++i]);
        } else if (!strcmp(a, "--triangles") && i + 1 < argc) {
            triangle_count = atoi(argv[++i]);
        } else if (!strcmp(a, "--scale") && i + 1 < argc) {
            scale_mult = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--size") && i + 1 < argc) {
            if (SDL_sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) {
                fprintf(stderr, "bad --size (want WxH)\n");
                return 1;
            }
        } else if (!strcmp(a, "--vsync")) {
            swap_interval = 1;
        } else if (!strcmp(a, "--no-vsync")) {
            swap_interval = 0;
        } else if (!strcmp(a, "--offload")) {
            force_offload = true;
        } else if (!strcmp(a, "--read-rgba")) {
            SDL_SetHint("SDLOP_GL_READ_FORMAT", "rgba");
        } else if (!strcmp(a, "--read-rgb")) {
            SDL_SetHint("SDLOP_GL_READ_FORMAT", "rgb");
        } else if (!strcmp(a, "--read-bgra")) {
            SDL_SetHint("SDLOP_GL_READ_FORMAT", "bgra");
        } else {
            fprintf(stderr, "Unknown/incomplete argument: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    if (triangle_count < 1) {
        triangle_count = 1;
    }
    if (win_w < 64 || win_h < 64) {
        win_w = 640;
        win_h = 480;
    }

    if (force_offload) {
        SDL_SetHint("SDLOP_GL_OFFLOAD", "force");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow("SDLop - GPU stress", win_w, win_h, SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(swap_interval);

    printf("GL_VERSION:  %s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VENDOR:   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("size: %dx%d  triangles/frame: %d  swap_interval: %d\n",
           win_w, win_h, triangle_count, swap_interval);

    GLuint bg_prog = build_program(bg_vs, bg_fs, "aPos", NULL);
    GLuint tri_prog = build_program(tri_vs, tri_fs, "aPos", "aColor");

    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    static const float verts[] = {
         0.0f,  0.6f, 1.0f, 0.2f, 0.2f,
        -0.6f, -0.5f, 0.2f, 1.0f, 0.3f,
         0.6f, -0.5f, 0.2f, 0.3f, 1.0f,
    };

    /* Deterministic scene layout. */
    Tri *tris = (Tri *)calloc((size_t)triangle_count, sizeof(Tri));
    if (!tris) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    for (int i = 0; i < triangle_count; i++) {
        double a = (double)i * 2.399963229728653; /* golden angle */
        double r = sqrt((double)(i + 1) / (double)triangle_count);
        tris[i].center[0] = (float)(cos(a) * r * 0.75);
        tris[i].center[1] = (float)(sin(a) * r * 0.75);
        tris[i].color[0] = (float)(0.5 + 0.5 * sin(a));
        tris[i].color[1] = (float)(0.5 + 0.5 * sin(a * 1.3 + 1.0));
        tris[i].color[2] = (float)(0.5 + 0.5 * sin(a * 0.7 + 2.0));
        tris[i].scale = (0.55f + 0.30f * (float)((i % 7) / 7.0)) * scale_mult;
        tris[i].spin = (float)(0.4 + (i % 11) * 0.15);
    }

    GLint bg_u_time = glGetUniformLocation(bg_prog, "uTime");
    GLint bg_u_res = glGetUniformLocation(bg_prog, "uRes");
    GLint tri_u_center = glGetUniformLocation(tri_prog, "uCenter");
    GLint tri_u_angle = glGetUniformLocation(tri_prog, "uAngle");
    GLint tri_u_scale = glGetUniformLocation(tri_prog, "uScale");
    GLint tri_u_res = glGetUniformLocation(tri_prog, "uRes");
    GLint tri_u_time = glGetUniformLocation(tri_prog, "uTime");

    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 t0 = SDL_GetPerformanceCounter();
    Uint64 last_report = t0, last_report_frames = 0, frames = 0;
    Uint64 draw_ns_total = 0, swap_ns_total = 0, finish_ns_total = 0;
    bool do_finish = getenv("GL_FINISH") != NULL;
    bool done = false;

    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
        }
        Uint64 now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - t0) / (double)freq;
        if (duration_sec > 0.0 && elapsed >= duration_sec) {
            break;
        }
        if (bench_frames > 0 && (int)frames >= bench_frames) {
            break;
        }

        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        if (pw <= 0 || ph <= 0) {
            pw = win_w;
            ph = win_h;
        }
        Uint64 draw_start = SDL_GetPerformanceCounter();

        glViewport(0, 0, pw, ph);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Procedural full-screen background. */
        glUseProgram(bg_prog);
        glUniform1f(bg_u_time, (float)elapsed);
        glUniform2f(bg_u_res, (float)pw, (float)ph);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* Many overlapping, alpha-blended spinning triangles. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(tri_prog);
        glUniform2f(tri_u_res, (float)pw, (float)ph);
        glUniform1f(tri_u_time, (float)elapsed);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts + 2);
        glEnableVertexAttribArray(1);
        for (int i = 0; i < triangle_count; i++) {
            glUniform2f(tri_u_center, tris[i].center[0], tris[i].center[1]);
            glUniform1f(tri_u_angle, (float)elapsed * tris[i].spin + (float)i * 0.37f);
            glUniform1f(tri_u_scale, tris[i].scale);
            glVertexAttrib3f(1, tris[i].color[0], tris[i].color[1], tris[i].color[2]);
            glDisableVertexAttribArray(1); /* use constant color per triangle */
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisableVertexAttribArray(1);
        Uint64 draw_end = SDL_GetPerformanceCounter();
        draw_ns_total += (draw_end - draw_start) * 1000000000ULL / freq;

        if (do_finish) {
            Uint64 f0 = SDL_GetPerformanceCounter();
            glFinish();
            Uint64 f1 = SDL_GetPerformanceCounter();
            finish_ns_total += (f1 - f0) * 1000000000ULL / freq;
        }

        Uint64 swap_start = SDL_GetPerformanceCounter();
        SDL_GL_SwapWindow(window);
        Uint64 swap_end = SDL_GetPerformanceCounter();
        swap_ns_total += (swap_end - swap_start) * 1000000000ULL / freq;

        frames++;
        now = SDL_GetPerformanceCounter();
        double rdt = (double)(now - last_report) / (double)freq;
        if (rdt >= 1.0) {
            Uint64 df = frames - last_report_frames;
            printf("FPS: %.2f (%llu frames in %.3fs)\n", (double)df / rdt,
                   (unsigned long long)df, rdt);
            fflush(stdout);
            last_report = now;
            last_report_frames = frames;
        }
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    double sec = (double)(t1 - t0) / (double)freq;
    if (sec > 0.0) {
        printf("TOTAL: %.2f fps (%llu frames in %.3fs)\n", (double)frames / sec,
               (unsigned long long)frames, sec);
        if (frames) {
            printf("avg draw submission: %.2f ms, avg glFinish: %.2f ms, avg SwapWindow: %.2f ms\n",
                   (double)draw_ns_total / (double)frames / 1e6,
                   (double)finish_ns_total / (double)frames / 1e6,
                   (double)swap_ns_total / (double)frames / 1e6);
        }
    }

    free(tris);
    glDeleteProgram(bg_prog);
    glDeleteProgram(tri_prog);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
