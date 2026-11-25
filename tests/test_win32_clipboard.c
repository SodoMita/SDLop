/*
  SDLop test: the Win32 clipboard, through SDL_*Clipboard*.

  With no driver hooks the clipboard is in-process only: SDL_SetClipboardText
  followed by SDL_GetClipboardText works, but no other application can see the
  data and text copied elsewhere never appears. This test therefore checks the
  platform clipboard directly, not just the round trip.

  What is covered:
    - text reaches CF_UNICODETEXT, readable by Win32 itself
    - UTF-8 survives the wide-char conversion (non-ASCII round trip)
    - image/png reaches a real clipboard format via a registered "PNG" format
    - CF_TEXT (ANSI) clipboard content is still readable as UTF-8
    - SDL_HasClipboardData agrees with the platform for present and absent types
    - text/html is offered as "HTML Format" with a parseable CF_HTML header

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

static const char *const two_types[] = { "text/plain;charset=utf-8", "text/html" };
static const char *const image_types[] = { "image/png" };

static const void *SDLCALL text_callback(void *userdata, const char *mime_type, size_t *size)
{
    (void)mime_type;
    *size = SDL_strlen((const char *)userdata);
    return (const char *)userdata;
}

static const void *SDLCALL html_callback(void *userdata, const char *mime_type, size_t *size)
{
    static const char html[] = "<b>bold</b>";

    (void)userdata;
    if (SDL_strcmp(mime_type, "text/html") == 0) {
        *size = sizeof(html) - 1;
        return html;
    }
    return text_callback(userdata, mime_type, size);
}

static const unsigned char png_bytes[] = {
    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, /* signature, enough to identify it */
    0x00, 0x01, 0x02, 0x03                      /* body */
};

static const void *SDLCALL png_callback(void *userdata, const char *mime_type, size_t *size)
{
    (void)userdata;
    (void)mime_type;
    *size = sizeof(png_bytes);
    return (const char *)png_bytes;
}

/* Read CF_UNICODETEXT straight from Win32, as another application would. */
static bool read_platform_text(wchar_t *out, size_t out_bytes)
{
    HANDLE handle;
    const wchar_t *wide;

    out[0] = L'\0';
    if (!OpenClipboard(NULL)) {
        return false;
    }
    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        wide = (const wchar_t *)GlobalLock(handle);
        if (wide) {
            wcsncpy(out, wide, out_bytes / sizeof(wchar_t) - 1);
            out[out_bytes / sizeof(wchar_t) - 1] = L'\0';
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return handle != NULL;
}

int main(void)
{
    SDL_Window *window;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 clipboard", 320, 200, 0);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* --- text, checked as the platform sees it ------------------------- */
    SDL_SetClipboardText("hello clipboard");
    {
        wchar_t platform[128];
        char *round_trip;

        CHECK(read_platform_text(platform, sizeof(platform)),
              "CF_UNICODETEXT is empty after SDL_SetClipboardText: the data never "
              "reached the Win32 clipboard");
        CHECK(wcscmp(platform, L"hello clipboard") == 0,
              "Win32 clipboard holds a different string than was set");

        round_trip = SDL_GetClipboardText();
        CHECK(round_trip && SDL_strcmp(round_trip, "hello clipboard") == 0,
              "round trip gave %s", round_trip ? round_trip : "(null)");
        SDL_free(round_trip);
    }

    /* --- non-ASCII must survive the wide-char conversion -------------- */
    SDL_SetClipboardText("caf\xC3\xA9 \xE2\x9C\x93"); /* "café ✓" */
    {
        wchar_t platform[128];
        char *round_trip = SDL_GetClipboardText();

        CHECK(read_platform_text(platform, sizeof(platform)), "no platform text");
        CHECK(round_trip && SDL_strcmp(round_trip, "caf\xC3\xA9 \xE2\x9C\x93") == 0,
              "UTF-8 did not survive: %s", round_trip ? round_trip : "(null)");
        SDL_free(round_trip);
    }

    /* --- a native (non-SDL) write is readable, including ANSI CF_TEXT --- */
    {
        static const wchar_t native[] = L"written by win32";
        HGLOBAL memory;
        wchar_t *locked;

        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(native));
            if (memory && (locked = (wchar_t *)GlobalLock(memory)) != NULL) {
                memcpy(locked, native, sizeof(native));
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            }
            CloseClipboard();
        }
        {
            char *text = SDL_GetClipboardText();
            CHECK(text && SDL_strcmp(text, "written by win32") == 0,
                  "SDL could not read text that Win32 put on the clipboard: %s",
                  text ? text : "(null)");
            SDL_free(text);
        }
    }

    /* --- CF_TEXT only (ANSI) ------------------------------------------ */
    {
        static const char ansi[] = "plain ansi";
        HGLOBAL memory;
        char *locked;

        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(ansi));
            if (memory && (locked = (char *)GlobalLock(memory)) != NULL) {
                memcpy(locked, ansi, sizeof(ansi));
                GlobalUnlock(memory);
                SetClipboardData(CF_TEXT, memory);
            }
            CloseClipboard();
        }
        {
            char *text = SDL_GetClipboardText();
            CHECK(text && SDL_strcmp(text, "plain ansi") == 0,
                  "CF_TEXT (ANSI) fallback gave %s", text ? text : "(null)");
            SDL_free(text);
        }
        CHECK(SDL_HasClipboardText(), "SDL_HasClipboardText disagreed with CF_TEXT");
    }

    /* --- audio/mpeg must be reported absent, not fabricated ------------- */
    CHECK(!SDL_HasClipboardData("audio/mpeg"),
          "SDL_HasClipboardData claimed audio/mpeg is available");

    SDL_ClearClipboardData();
    CHECK(!SDL_HasClipboardText(), "SDL_HasClipboardText after clearing");
    CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE,
          "CF_UNICODETEXT survived SDL_ClearClipboardData");

    /* --- image/png reaches a real, registered clipboard format --------- */
    {
        UINT png_format;
        bool reached;

        SDL_SetClipboardData(png_callback, NULL, NULL, image_types, 1);
        CHECK(SDL_HasClipboardData("image/png"), "SDL_HasClipboardData(image/png) is false "
                                                 "right after offering one");
        png_format = RegisterClipboardFormatW(L"PNG");
        reached = (png_format && IsClipboardFormatAvailable(png_format)) ? true : false;
        CHECK(reached, "the PNG data never reached the Win32 clipboard");
        {
            size_t size = 0;
            void *data = SDL_GetClipboardData("image/png", &size);
            CHECK(data && size == sizeof(png_bytes),
                  "read back %u bytes, want %u", (unsigned)size, (unsigned)sizeof(png_bytes));
            CHECK(data && SDL_memcmp(data, png_bytes, sizeof(png_bytes)) == 0,
                  "the PNG bytes changed on the way through the clipboard");
            SDL_free(data);
        }
    }

    /* --- text/html as CF_HTML ------------------------------------------ */
    {
        UINT html_format;
        bool reached = false;

        SDL_SetClipboardData(html_callback, NULL, (void *)"plain", two_types, 2);
        CHECK(SDL_HasClipboardData("text/plain;charset=utf-8"), "text/plain not offered");
        html_format = RegisterClipboardFormatW(L"HTML Format");
        CHECK(html_format != 0, "could not register HTML Format");
        if (html_format) {
            HANDLE handle;
            reached = IsClipboardFormatAvailable(html_format) ? true : false;
            CHECK(reached, "text/html never reached the Win32 clipboard as HTML Format");
            if (reached && OpenClipboard(NULL)) {
                handle = GetClipboardData(html_format);
                if (handle) {
                    const char *html = (const char *)GlobalLock(handle);
                    if (html) {
                        CHECK(SDL_strstr(html, "Version:0.9") != NULL,
                              "CF_HTML header missing");
                        CHECK(SDL_strstr(html, "<b>bold</b>") != NULL,
                              "the HTML payload is not in the CF_HTML blob");
                        CHECK(SDL_strstr(html, "StartFragment:") != NULL,
                              "CF_HTML offsets missing");
                        GlobalUnlock(handle);
                    }
                }
                CloseClipboard();
            }
        }
    }

    /* --- a change made by another application is reported --------------
     * Win32 sends no notification, so the driver compares the clipboard
     * sequence number on every pump. Without that, an app keeps reading the
     * stale string after the user copies something elsewhere. */
    {
        static const wchar_t native[] = L"from another application";
        HGLOBAL memory;
        wchar_t *locked;
        SDL_Event event;
        int updates = 0;
        int owner_flag = -1;
        int i;

        SDL_SetClipboardData(text_callback, NULL, (void *)"ours", two_types, 1);
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            /* drop anything already queued */
        }

        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(native));
            if (memory && (locked = (wchar_t *)GlobalLock(memory)) != NULL) {
                memcpy(locked, native, sizeof(native));
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            }
            CloseClipboard();
        }
        for (i = 0; i < 30; i++) {
            SDL_PumpEvents();
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_CLIPBOARD_UPDATE) {
                    updates++;
                    owner_flag = event.clipboard.owner ? 1 : 0;
                }
            }
            SDL_Delay(10);
        }
        CHECK(updates >= 1, "a clipboard change made by another application produced no "
                            "SDL_EVENT_CLIPBOARD_UPDATE");
        CHECK(owner_flag == 0, "foreign change reported as our own offer (owner=1)");
        {
            char *now = SDL_GetClipboardText();
            CHECK(now && SDL_strcmp(now, "from another application") == 0,
                  "stale clipboard text after a foreign change: %s", now ? now : "(null)");
            SDL_free(now);
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_clipboard: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_clipboard: text, ANSI fallback, PNG, CF_HTML, and foreign changes detected\n");
    return 0;
}
