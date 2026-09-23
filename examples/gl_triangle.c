/*
  SDLop example: OpenGL ES 2 triangle rendered through EGL on Wayland
  (software rasterized by Mesa llvmpipe on GPU-less systems).

  Usage: gl_triangle [frames]   (pass a frame count to benchmark)
*/

#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>

static const char *vs_src =
    "attribute vec2 aPos;"
    "attribute vec3 aColor;"
    "varying vec3 vColor;"
    "void main() { vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }";

static const char *fs_src =
    "precision mediump float;"
    "varying vec3 vColor;"
    "void main() { gl_FragColor = vec4(vColor, 1.0); }";

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

int main(int argc, char *argv[])
{
    int bench_frames = 0;
    if (argc > 1) {
        bench_frames = atoi(argv[1]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow("SDLop - GL triangle (llvmpipe)", 640, 480, SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(0);

    printf("GL_VERSION:   %s\n", (const char *)glGetString(GL_VERSION));
    printf("GL_RENDERER:  %s\n", (const char *)glGetString(GL_RENDERER));

    GLuint prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aColor");
    glLinkProgram(prog);
    glUseProgram(prog);

    static const float verts[] = {
        /* x,     y,     r,   g,   b */
        0.0f,  0.8f,  1.0f, 0.0f, 0.0f,
        -0.8f, -0.8f, 0.0f, 1.0f, 0.0f,
        0.8f, -0.8f,  0.0f, 0.0f, 1.0f,
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(1);

    bool done = false;
    int frames = 0;
    Uint64 t0 = SDL_GetTicksNS();
    while (!done && (!bench_frames || frames < bench_frames)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
        }
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SDL_GL_SwapWindow(window);
        frames++;
        if (!bench_frames) {
            SDL_Delay(16);
        }
    }
    Uint64 dt = SDL_GetTicksNS() - t0;

    /* self-check: read back a pixel from the triangle center */
    Uint8 px[4] = { 0 };
    glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("center pixel: %u %u %u %u\n", px[0], px[1], px[2], px[3]);

    if (bench_frames) {
        printf("GL (llvmpipe): %d frames in %.3f s => %.1f fps\n",
               frames, dt / 1e9, frames / (dt / 1e9));
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
