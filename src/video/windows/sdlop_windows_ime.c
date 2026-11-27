/*
  SDLop - Win32 IME composition (Imm32): SDL_EVENT_TEXT_EDITING and
  SDL_EVENT_TEXT_EDITING_CANDIDATES.

  Text *input* already works without this: when an IME commits a composition,
  Windows delivers the finished characters as WM_CHAR and the driver turns them
  into SDL_EVENT_TEXT_INPUT. What was missing is the composition itself, so an
  application that shows an in-progress composition (a search box, a chat
  input, a renamed item) had nothing to display and no way to know that a
  composition was running at all.

  The message flow, as stock SDL3 handles it in SDL_windowskeyboard.c:

      WM_IME_STARTCOMPOSITION              composition began
      WM_IME_COMPOSITION (GCS_COMPSTR)     the string + cursor position
      WM_IME_COMPOSITION (GCS_RESULTSTR)   accepted; also arrives as WM_CHAR
      WM_IME_ENDCOMPOSITION                abandoned or finished
      WM_IME_NOTIFY (IMN_*CANDIDATE*)      the candidate list

  Nothing is sent unless text input is active on the focused window --
  SDLOP_SendEditingText and SDLOP_SendEditingTextCandidates check that
  themselves, because an IME must not compose into a window that is not
  accepting text.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "sdlop_windows.h"

#include <imm.h>

/* UTF-16 (from Imm32) to UTF-8, through the same Win32 call the WM_CHAR path
 * uses. Returns a malloced string, or NULL. */
static char *ime_to_utf8(const WCHAR *wide, int wchars)
{
    int bytes;
    char *utf8;

    if (wchars <= 0 || !wide) {
        return NULL;
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide, wchars, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
        return NULL;
    }
    utf8 = (char *)SDL_malloc((size_t)bytes + 1);
    if (!utf8) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, wchars, utf8, bytes, NULL, NULL) != bytes) {
        SDL_free(utf8);
        return NULL;
    }
    utf8[bytes] = '\0';
    return utf8;
}

/* Imm32 can report the same composition more than once for one change: Wine
 * emits its own WM_IME_STARTCOMPOSITION/WM_IME_COMPOSITION pair when
 * ImmSetCompositionString runs, and the application then processes a second
 * message for the same string. Measuring drove this: one posted
 * WM_IME_COMPOSITION produced three editing events (an empty start, then the
 * string, then the string again). Re-sending an identical string is noise the
 * application has to filter, so it is filtered here instead -- what counts is
 * the text and the caret, not how many messages said so. */
static char ime_last_text[256];
static int ime_last_caret = -1;
static HWND ime_last_hwnd;

/* Two windows must never share the de-duplication state: the second one would
 * never see its first composition if it happened to match the first one's. */
static void ime_forget_if_other_window(HWND hwnd)
{
    if (ime_last_hwnd != hwnd) {
        ime_last_hwnd = hwnd;
        ime_last_text[0] = '\0';
        ime_last_caret = -1;
    }
}

static void ime_send_if_changed(const char *utf8, int caret)
{
    /* Only a non-empty composition is de-duplicated. "Composition is over" is
     * always delivered, even twice: it is the event that unsticks an
     * application showing composition state, and swallowing the second one
     * left a window with no editing text but a stale "composing" UI. */
    if (utf8[0] != '\0' && ime_last_caret == caret && SDL_strcmp(ime_last_text, utf8) == 0) {
        return;
    }
    SDL_strlcpy(ime_last_text, utf8, sizeof(ime_last_text));
    ime_last_caret = caret;
    SDLOP_SendEditingText(utf8, caret, 0);
}

/* The composition string, plus where the caret sits inside it. The caret is in
 * UTF-16 code units from Imm32 and in UTF-8 bytes in the SDL event, so the part
 * before it is converted to measure it. */
static void ime_send_composition(HWND hwnd)
{
    HIMC himc = ImmGetContext(hwnd);
    LONG wlen;
    LONG caret;
    WCHAR *wide;
    char *utf8;
    int caret_bytes = 0;

    if (!himc) {
        return;
    }
    wlen = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
    if (wlen < 0) {
        ImmReleaseContext(hwnd, himc);
        return;
    }
    wide = (WCHAR *)SDL_malloc((size_t)wlen + sizeof(WCHAR));
    if (!wide) {
        ImmReleaseContext(hwnd, himc);
        return;
    }
    if (wlen > 0 && ImmGetCompositionStringW(himc, GCS_COMPSTR, wide, (DWORD)wlen) < 0) {
        SDL_free(wide);
        ImmReleaseContext(hwnd, himc);
        return;
    }
    wide[wlen / (LONG)sizeof(WCHAR)] = 0;

    caret = ImmGetCompositionStringW(himc, GCS_CURSORPOS, NULL, 0);
    if (caret > 0) {
        const int wchars = (int)(caret / (LONG)sizeof(WCHAR));
        char *before = ime_to_utf8(wide, wchars);
        if (before) {
            caret_bytes = (int)SDL_strlen(before);
            SDL_free(before);
        }
    }
    ImmReleaseContext(hwnd, himc);

    utf8 = ime_to_utf8(wide, (int)(wlen / (LONG)sizeof(WCHAR)));
    SDL_free(wide);
    /* an empty composition still has to be reported: that is how the
     * application learns the composition was cleared */
    ime_send_if_changed(utf8 ? utf8 : "", caret_bytes);
    SDL_free(utf8);
}

/* The candidate list the IME is offering. Imm32 hands it over as a list of
 * UTF-16 strings in one buffer, each NUL terminated; SDL takes an array of
 * UTF-8 strings plus which one is selected. */
static void ime_send_candidates(HWND hwnd, WPARAM wparam)
{
    HIMC himc = ImmGetContext(hwnd);
    DWORD bytes;
    DWORD count = 0;
    DWORD index = 0;
    bool vertical_select = false;
    char **list;
    WCHAR *buf;
    DWORD i;
    const WCHAR *cursor;

    if (!himc) {
        return;
    }
    bytes = ImmGetCandidateListW(himc, 0, NULL, 0);
    if (bytes == 0) {
        /* no list open: an empty candidate list closes the UI */
        ImmReleaseContext(hwnd, himc);
        SDLOP_SendEditingTextCandidates(NULL, 0, 0, false);
        return;
    }
    buf = (WCHAR *)SDL_malloc(bytes);
    if (!buf) {
        ImmReleaseContext(hwnd, himc);
        return;
    }
    if (ImmGetCandidateListW(himc, 0, (CANDIDATELIST *)buf, bytes) == 0) {
        SDL_free(buf);
        ImmReleaseContext(hwnd, himc);
        return;
    }
    {
        const CANDIDATELIST *cl = (const CANDIDATELIST *)buf;
        count = cl->dwCount;
        index = cl->dwSelection;
        /* IMN_*CANDIDATEOPEN/CLOSE tell the application whether the list runs
         * horizontally; SDL wants that as a bool */
        vertical_select = (wparam == IMN_OPENCANDIDATE) ? false : true;
        if (count > 0) {
            list = (char **)SDL_calloc(count, sizeof(char *));
            if (!list) {
                SDL_free(buf);
                ImmReleaseContext(hwnd, himc);
                return;
            }
            for (i = 0; i < count; ++i) {
                cursor = (const WCHAR *)((const char *)buf + cl->dwOffset[i]);
                /* each entry is NUL terminated UTF-16; the bytes available
                 * are whatever follows, so measure in WCHARs */
                list[i] = ime_to_utf8(cursor, (int)SDL_wcslen(cursor));
                if (!list[i]) {
                    list[i] = SDL_strdup("");
                }
            }
            SDLOP_SendEditingTextCandidates(list, (int)count, (int)index, !vertical_select);
            for (i = 0; i < count; ++i) {
                SDL_free(list[i]);
            }
            SDL_free(list);
        } else {
            SDLOP_SendEditingTextCandidates(NULL, 0, 0, false);
        }
    }
    SDL_free(buf);
    ImmReleaseContext(hwnd, himc);
}

bool SDLOP_Win32IMEHandleMessage(SDL_Window *window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    HIMC himc;

    if (!window || !window->driverdata) {
        return false;
    }
    {
        SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
        if (!data->hwnd) {
            return false;
        }
        ime_forget_if_other_window(data->hwnd);
        switch (msg) {
        case WM_IME_STARTCOMPOSITION:
            ime_send_if_changed("", 0);
            return true;

        case WM_IME_COMPOSITION:
            if (lparam & GCS_COMPSTR) {
                ime_send_composition(data->hwnd);
                return true;
            }
            /* GCS_RESULTSTR is the committed string; Windows also sends it as
             * WM_CHAR, so sending text here as well would double every
             * character. The composition is over, so clear the editing text. */
            if (lparam & GCS_RESULTSTR) {
                ime_send_if_changed("", 0);
                return true;
            }
            return false;

        case WM_IME_ENDCOMPOSITION:
            ime_send_if_changed("", 0);
            return true;

        case WM_IME_NOTIFY:
            if (wparam == IMN_OPENCANDIDATE || wparam == IMN_CHANGECANDIDATE ||
                wparam == IMN_CLOSECANDIDATE) {
                ime_send_candidates(data->hwnd, wparam);
                return true;
            }
            if (wparam == IMN_SETOPENSTATUS) {
                /* the IME was switched on or off: SDL reports that as a
                 * composition boundary, so an application can show or hide
                 * its own composition UI */
                himc = ImmGetContext(data->hwnd);
                if (himc) {
                    const bool open = ImmGetOpenStatus(himc) != FALSE;
                    ImmReleaseContext(data->hwnd, himc);
                    if (!open) {
                        ime_send_if_changed("", 0);
                    }
                }
                return true;
            }
            return false;

        default:
            return false;
        }
    }
}

#endif /* _WIN32 */
