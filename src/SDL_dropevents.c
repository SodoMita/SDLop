/*
  SDLop - drag-and-drop events (port of stock SDL3's SDL_dropevents.c and
  the URI helpers of SDL_utils.c).

  Drivers report a drop as SDLOP_SendDropPosition()* then
  SDLOP_SendDropFile()/SDLOP_SendDropText()* and SDLOP_SendDropComplete();
  SDL_EVENT_DROP_BEGIN is synthesised before the first event of a drop,
  exactly as stock does. Windows accept drops while SDL_EVENT_DROP_FILE or
  SDL_EVENT_DROP_TEXT is enabled (driver hook AcceptDragAndDrop).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#include <string.h>
#ifdef _WIN32
  #include <winsock2.h>   /* gethostname */
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static bool app_is_dropping = false;
static float last_drop_x = 0;
static float last_drop_y = 0;

static char *temporary_string(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)SDLOP_AllocateTemporaryMemory(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

static bool send_drop(SDL_Window *window, SDL_EventType type, const char *source, const char *data, float x, float y)
{
    bool posted = false;
    if (!SDL_EventEnabled(type)) {
        return false;
    }
    const bool need_begin = window ? !window->is_dropping : !app_is_dropping;
    SDL_Event event;
    if (need_begin) {
        SDL_zero(event);
        event.type = SDL_EVENT_DROP_BEGIN;
        event.drop.windowID = window ? window->id : 0;
        if (!SDL_PushEvent(&event)) {
            return false;
        }
        if (window) {
            window->is_dropping = true;
        } else {
            app_is_dropping = true;
        }
    }

    SDL_zero(event);
    event.type = type;
    if (source && !(event.drop.source = temporary_string(source))) {
        return false;
    }
    if (data && !(event.drop.data = temporary_string(data))) {
        return false;
    }
    event.drop.windowID = window ? window->id : 0;
    if (type == SDL_EVENT_DROP_POSITION) {
        last_drop_x = x;
        last_drop_y = y;
    }
    event.drop.x = last_drop_x;
    event.drop.y = last_drop_y;
    posted = SDL_PushEvent(&event);

    if (posted && type == SDL_EVENT_DROP_COMPLETE) {
        if (window) {
            window->is_dropping = false;
        } else {
            app_is_dropping = false;
        }
        last_drop_x = 0;
        last_drop_y = 0;
    }
    return posted;
}

bool SDLOP_SendDropFile(SDL_Window *window, const char *source, const char *file)
{
    return send_drop(window, SDL_EVENT_DROP_FILE, source, file, 0, 0);
}

bool SDLOP_SendDropPosition(SDL_Window *window, float x, float y)
{
    return send_drop(window, SDL_EVENT_DROP_POSITION, NULL, NULL, x, y);
}

bool SDLOP_SendDropText(SDL_Window *window, const char *text)
{
    return send_drop(window, SDL_EVENT_DROP_TEXT, NULL, text, 0, 0);
}

bool SDLOP_SendDropComplete(SDL_Window *window)
{
    return send_drop(window, SDL_EVENT_DROP_COMPLETE, NULL, NULL, 0, 0);
}

void SDLOP_SendDropPayload(SDL_Window *window, bool uri_list, char *data)
{
    char *save = NULL;
    for (char *tok = data ? SDL_strtok_r(data, "\r\n", &save) : NULL; tok; tok = SDL_strtok_r(NULL, "\r\n", &save)) {
        if (uri_list) {
            if (tok[0] == '#') {
                continue; /* RFC 2483 comment line */
            }
            if (SDLOP_URIToLocal(tok, tok) >= 0) {
                SDLOP_SendDropFile(window, NULL, tok);
            }
        } else {
            SDLOP_SendDropText(window, tok);
        }
    }
    SDLOP_SendDropComplete(window);
}

bool SDLOP_IsAcceptingDragAndDrop(void)
{
    return SDL_EventEnabled(SDL_EVENT_DROP_FILE) || SDL_EventEnabled(SDL_EVENT_DROP_TEXT);
}

void SDLOP_ToggleDragAndDropSupport(void)
{
    if (sdlop.video && sdlop.video->AcceptDragAndDrop) {
        const bool enable = SDLOP_IsAcceptingDragAndDrop();
        for (SDL_Window *w = sdlop.windows; w; w = w->next) {
            sdlop.video->AcceptDragAndDrop(w, enable);
        }
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* percent-decoding; dst may alias src (output is never longer) */
static int uri_decode(const char *src, char *dst)
{
    int len = (int)strlen(src);
    int wi = 0;
    for (int ri = 0; ri < len; ri++) {
        if (src[ri] == '%' && ri + 2 < len && hex_value(src[ri + 1]) >= 0 && hex_value(src[ri + 2]) >= 0) {
            dst[wi++] = (char)(hex_value(src[ri + 1]) * 16 + hex_value(src[ri + 2]));
            ri += 2;
        } else {
            dst[wi++] = src[ri];
        }
    }
    dst[wi] = '\0';
    return wi;
}

/* file: URI -> local path, in place allowed; -1 when not a local file
 * (same rules as stock's SDL_URIToLocal: file:/p, file:///p,
 * file://localhost/p, file://<this host>/p) */
int SDLOP_URIToLocal(const char *src, char *dst)
{
    if (memcmp(src, "file:/", 6) == 0) {
        src += 6;
    } else if (strstr(src, ":/") != NULL) {
        return -1; /* other scheme */
    }
    bool local = src[0] != '/' || (src[0] != '\0' && src[1] == '/');
    if (!local && src[0] == '/' && src[2] != '/') {
        const char *host_end = strchr(src + 1, '/');
        if (host_end) {
            const size_t n = (size_t)(host_end - (src + 1));
            char hostname[257];
            if (gethostname(hostname, 255) == 0) {
                hostname[256] = '\0';
                if (strlen(hostname) == n && SDL_strncasecmp(src + 1, hostname, n) == 0) {
                    src = host_end + 1;
                    local = true;
                }
            }
            if (!local && n == 9 && SDL_strncasecmp(src + 1, "localhost", 9) == 0) {
                src = host_end + 1;
                local = true;
            }
        }
    }
    if (!local) {
        return -1;
    }
    if (src[0] == '/') {
        src++;
    } else {
        src--;
    }
    return uri_decode(src, dst);
}
