/*
  SDLop test: the generated headers compile as C++ (-Wall -Wextra -pedantic)
  and every declaration has C linkage. A function from each header family is
  referenced, so a missing extern "C" shows up as an undefined
  (C++-mangled) symbol at link time.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>
#include <cstdio>
#include <cstring>

int main()
{
    if (!SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy")) {
        return 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("c++", 64, 64, 0);
    SDL_Surface *s = w ? SDL_GetWindowSurface(w) : nullptr;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, "n", 1);
    SDL_Mutex *m = SDL_CreateMutex();
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
    SDL_DestroyMutex(m);
    SDL_TimerID t = SDL_AddTimer(1000, [](void *, SDL_TimerID, Uint32) -> Uint32 { return 0; }, nullptr);
    SDL_RemoveTimer(t);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
    while (SDL_PollEvent(&e)) {
    }
    char buf[64];
    SDL_snprintf(buf, sizeof(buf), "%d", SDL_GetVersion());
    SDL_Log("C++ ok: version %s, surface %p, revision %s", buf, static_cast<void *>(s), SDL_GetRevision());
    bool ok = w && s && SDL_GetNumberProperty(props, "n", 0) == 1 && std::strlen(buf) > 0 &&
              SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, false) == SDLK_A;
    SDL_DestroyProperties(props);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return ok ? 0 : 1;
}
