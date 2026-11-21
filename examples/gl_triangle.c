/*
  SDLop example: spinning OpenGL ES 2 triangle.

  Similar in spirit to weston-simple-egl: renders a spinning RGB triangle,
  reports FPS periodically, and can run for a fixed duration.

  Usage:
    gl_triangle [--duration SECONDS] [--frames N] [--no-vsync]

  Examples:
    gl_triangle --duration 10
    gl_triangle --frames 600
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

static const char *vs_src =
    "attribute vec2 aPos;\n"
    "attribute vec3 aColor;\n"
    "uniform float uAngle;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "    float c = cos(uAngle);\n"
    "    float s = sin(uAngle);\n"
    "    vec2 p = vec2(aPos.x * c - aPos.y * s, aPos.x * s + aPos.y * c);\n"
    "    vColor = aColor;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(vColor, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "shader error: %s\n", log);
    }
    return sh;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--duration SECONDS] [--frames N] [--no-vsync]\n"
            "  --duration, -d SECONDS  Run for this many seconds (default: unlimited)\n"
            "  --frames,   -f N        Stop after N frames\n"
            "  --no-vsync              Disable vsync/swap interval (default)\n"
            "  --vsync                 Enable vsync (swap interval 1)\n"
            "  --help, -h              Show this help\n",
            argv0);
}

int main(int argc, char *argv[])
{
    int bench_frames = 0;
    double duration_sec = 0.0;
    int swap_interval = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (SDL_strcmp(arg, "--help") == 0 || SDL_strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if ((SDL_strcmp(arg, "--duration") == 0 || SDL_strcmp(arg, "-d") == 0) && i + 1 < argc) {
            duration_sec = atof(argv[++i]);
        } else if ((SDL_strcmp(arg, "--frames") == 0 || SDL_strcmp(arg, "-f") == 0) && i + 1 < argc) {
            bench_frames = atoi(argv[++i]);
        } else if (SDL_strcmp(arg, "--no-vsync") == 0) {
            swap_interval = 0;
        } else if (SDL_strcmp(arg, "--vsync") == 0) {
            swap_interval = 1;
        } else if (arg[0] != '-') {
            /* Backward compat: positional arg is frame count. */
            bench_frames = atoi(arg);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            usage(argv[0]);
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow("SDLop - spinning GL triangle",
                                          640, 480, SDL_WINDOW_OPENGL);
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

    if (!SDL_GL_SetSwapInterval(swap_interval)) {
        fprintf(stderr, "SDL_GL_SetSwapInterval(%d) warning: %s\n", swap_interval, SDL_GetError());
    }

    printf("GL_VERSION:  %s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VENDOR:   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("swap_interval: %d\n", swap_interval);
    if (duration_sec > 0.0) {
        printf("duration: %.3f s\n", duration_sec);
    }
    if (bench_frames > 0) {
        printf("frames: %d\n", bench_frames);
    }

    GLuint prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aColor");
    glLinkProgram(prog);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "program link error: %s\n", log);
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    glUseProgram(prog);
    GLint u_angle = glGetUniformLocation(prog, "uAngle");

    static const float verts[] = {
        /* x,     y,     r,   g,   b */
         0.0f,  0.8f,  1.0f, 0.0f, 0.0f,
        -0.8f, -0.8f,  0.0f, 1.0f, 0.0f,
         0.8f, -0.8f,  0.0f, 0.0f, 1.0f,
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(1);

    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 t0 = SDL_GetPerformanceCounter();
    Uint64 last_report = t0;
    Uint64 last_report_frames = 0;
    Uint64 total_frames = 0;
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
        if (bench_frames > 0 && (int)total_frames >= bench_frames) {
            break;
        }

        float angle = (float)(elapsed * M_PI * 0.5); /* 90 degrees/sec */
        glViewport(0, 0, 640, 480);
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w > 0 && h > 0) {
            glViewport(0, 0, w, h);
        }

        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(u_angle, angle);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SDL_GL_SwapWindow(window);

        total_frames++;
        now = SDL_GetPerformanceCounter();
        double report_dt = (double)(now - last_report) / (double)freq;
        if (report_dt >= 1.0) {
            Uint64 delta_frames = total_frames - last_report_frames;
            printf("FPS: %.2f (%llu frames in %.3fs)\n",
                   (double)delta_frames / report_dt,
                   (unsigned long long)delta_frames,
                   report_dt);
            last_report = now;
            last_report_frames = total_frames;
        }
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    double total_sec = (double)(t1 - t0) / (double)freq;
    if (total_sec > 0.0) {
        printf("TOTAL: %.2f fps (%llu frames in %.3fs)\n",
               (double)total_frames / total_sec,
               (unsigned long long)total_frames,
               total_sec);
    }

    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
