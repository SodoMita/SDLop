/*
  SDLop test: Win32 filesystem and clipboard ownership.

  These are the paths that had no Windows test at all, which is why three
  filesystem bugs and a clipboard bug survived this long:

    - SDL_RemovePath could not delete a *file*. It called RemoveDirectoryW
      first and only fell back to DeleteFileW on ERROR_ACCESS_DENIED, but that
      call fails with ERROR_DIRECTORY (267) on a regular file, so the fallback
      never ran. It also reported success for a missing parent directory.
    - SDL_CreateDirectory passed forward-slashed and possibly relative paths
      to SHCreateDirectoryExW, a Shell API that wants backslashes and a full
      path, so it failed with ERROR_BAD_PATHNAME -- including every
      SDL_GetPrefPath() caller.
    - SDL_GetUserFolder ran the XDG code on Windows, so it read
      $HOME/.config/user-dirs.dirs (absent) and reported Saved Games and
      Screenshots as "unavailable on XDG".
    - SDL_SetClipboardData left win32_clipboard_sequence stale, so the next
      SDL_PumpEvents saw its own write as a foreign change and cancelled the
      application's offer.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <windows.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            failures++;                                         \
        }                                                       \
    } while (0)

static bool file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int main(int argc, char *argv[])
{
    char *base;

    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "windows");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    /* ---- SDL_GetPrefPath: exercises SDL_CreateDirectory with a full path ---- */
    base = SDL_GetPrefPath("SDLopTest", "fswin");
    CHECK(base != NULL, "SDL_GetPrefPath: %s", SDL_GetError());
    if (base) {
        CHECK(file_exists(base), "SDL_GetPrefPath did not create its directory: %s", base);

        /* ---- SDL_RemovePath must delete a regular file ---- */
        {
            char path[1024];
            SDL_snprintf(path, sizeof(path), "%sremove_me.txt", base);
            FILE *f = fopen(path, "wb");
            CHECK(f != NULL, "could not create %s", path);
            if (f) {
                fputs("x", f);
                fclose(f);
            }
            CHECK(file_exists(path), "test file was not created");
            CHECK(SDL_RemovePath(path), "SDL_RemovePath on a regular file failed: %s", SDL_GetError());
            CHECK(!file_exists(path), "file still exists after SDL_RemovePath");
            /* already gone: success, as stock SDL3 */
            CHECK(SDL_RemovePath(path), "SDL_RemovePath on a missing file should succeed: %s", SDL_GetError());
            /* a missing parent must be a failure, not a silent success */
            {
                char nowhere[1024];
                SDL_snprintf(nowhere, sizeof(nowhere), "%snope/deeper/x.txt", base);
                CHECK(!SDL_RemovePath(nowhere), "SDL_RemovePath of a path whose parent is missing should fail");
            }
        }

        /* ---- SDL_CreateDirectory: nested, forward-slashed ----
         * The path has to be fresh every run: a leftover directory makes the
         * Shell API return ERROR_ALREADY_EXISTS and hides the real failure. */
        {
            char deep[1024];
            SDL_snprintf(deep, sizeof(deep), "%sfresh_%lu/a/b/c", base, (unsigned long)GetTickCount());
            CHECK(SDL_CreateDirectory(deep), "SDL_CreateDirectory(\"a/b/c\") failed: %s", SDL_GetError());
            CHECK(file_exists(deep), "nested directory was not created: %s", deep);
            CHECK(SDL_CreateDirectory(deep), "SDL_CreateDirectory on an existing directory must succeed");
            /* SDL_RemovePath must refuse a *non-empty* directory and leave it
             * in place. This used to point at base + "a", a directory nothing
             * ever created, so the call legitimately succeeded on a missing
             * path and the check was asserting the wrong thing -- invisible
             * on a prefix where a stray "a" happened to exist. Build the
             * directory here instead of assuming one. */
            {
                char nd[1024], inner[1024];
                SDL_snprintf(nd, sizeof(nd), "%snotempty_%lu", base, (unsigned long)GetTickCount());
                CHECK(SDL_CreateDirectory(nd), "SDL_CreateDirectory(%s): %s", nd, SDL_GetError());
                SDL_snprintf(inner, sizeof(inner), "%s/inside.txt", nd);
                {
                    FILE *f = fopen(inner, "wb");
                    CHECK(f != NULL, "could not create %s", inner);
                    if (f) {
                        fputs("x", f);
                        fclose(f);
                    }
                }
                CHECK(!SDL_RemovePath(nd), "SDL_RemovePath of a non-empty directory should fail");
                CHECK(file_exists(nd), "a refused removal must leave the directory alone: %s", nd);
                /* delete the contents, then the now-empty directory itself */
                CHECK(SDL_RemovePath(inner), "SDL_RemovePath(%s): %s", inner, SDL_GetError());
                CHECK(SDL_RemovePath(nd), "SDL_RemovePath of an empty directory should succeed: %s", SDL_GetError());
                CHECK(!file_exists(nd), "empty directory still exists after SDL_RemovePath");
            }
        }
        SDL_free(base);
    }

    /* ---- SDL_GetUserFolder comes from the Shell, not XDG ---- */
    {
        const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
        CHECK(home != NULL, "SDL_FOLDER_HOME: %s", SDL_GetError());
        if (home) {
            const size_t len = SDL_strlen(home);
            CHECK(len > 0 && home[len - 1] == '/', "folder must end with '/': %s", home);
            CHECK(SDL_strstr(home, ".config") == NULL, "HOME still came from XDG: %s", home);
            CHECK(SDL_strchr(home, '\\') == NULL || len == 0, "path not normalised to '/': %s", home);
        }
        /* these two were hard-coded to fail with "unavailable on XDG" */
        CHECK(SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS) != NULL, "SDL_FOLDER_DOCUMENTS: %s", SDL_GetError());
        CHECK(SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS) != NULL, "SDL_FOLDER_DOWNLOADS: %s", SDL_GetError());
    }

    /* ---- an own clipboard write must not come back as a foreign change ---- */
    {
        SDL_Event event;
        bool saw_text = false;
        CHECK(SDL_SetClipboardText("sdlop-self-write"), "SDL_SetClipboardText: %s", SDL_GetError());
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_CLIPBOARD_UPDATE) {
                /* The set itself reports one update, and it carries the
                 * application's offer. A driver that mistook its own write
                 * for a foreign change reported zero MIME types after
                 * cancelling the offer. */
                CHECK(event.clipboard.num_mime_types > 0,
                      "a clipboard update with no MIME types: the driver treated its own write as foreign");
                saw_text = true;
            }
        }
        CHECK(saw_text, "SDLK_SetClipboardText reported no clipboard update at all");
        CHECK(SDL_HasClipboardText(), "text not readable back");
        {
            char *text = SDL_GetClipboardText();
            CHECK(text && SDL_strcmp(text, "sdlop-self-write") == 0,
                  "clipboard text round-trip: got '%s'", text ? text : "(null)");
            SDL_free(text);
        }
        /* a second pump must be quiet: nothing changed since the write */
        {
            bool any = false;
            SDL_PumpEvents();
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_CLIPBOARD_UPDATE) {
                    any = true;
                }
            }
            CHECK(!any, "a second SDL_PumpEvents reported another clipboard update");
        }
    }

    SDL_Quit();

    if (failures) {
        printf("test_win32_fileops: FAIL (%d)\n", failures);
        return 1;
    }
    printf("test_win32_fileops: PASS\n");
    return 0;
}
