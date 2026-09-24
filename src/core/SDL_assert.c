/*
  SDLop -- SDL_assert.h.

  Upstream's assertion macros (copied into the header verbatim) call
  SDL_ReportAssertion(); this is the small runtime behind them. SDLop has no
  assertion report list, so SDL_ResetAssertionReport() has nothing to clear.
*/

#include "../sdlop_internal.h"

#include <stdio.h>
#include <stdlib.h>

static SDL_AssertionHandler sdlop_assert_handler;
static void *sdlop_assert_handler_userdata;

static SDL_AssertState SDLCALL sdlop_default_assert_handler(const SDL_AssertData *data, void *userdata)
{
    (void)userdata;
    fprintf(stderr, "Assertion failure: %s\n", data->condition ? data->condition : "(unknown)");
    fflush(stderr);
    return SDL_ASSERTION_ABORT;
}

SDL_AssertState SDL_ReportAssertion(SDL_AssertData *data, const char *func, const char *file, int line)
{
    SDL_AssertionHandler handler;
    SDL_AssertState state;

    if (!data) {
        return SDL_ASSERTION_IGNORE;
    }
    if (data->always_ignore) {
        return SDL_ASSERTION_IGNORE;
    }

    data->trigger_count++;
    if (data->trigger_count == 1) {
        data->filename = file;
        data->linenum = line;
        data->function = func;
    }

    handler = sdlop_assert_handler ? sdlop_assert_handler : sdlop_default_assert_handler;
    state = handler(data, sdlop_assert_handler_userdata);
    if (state == SDL_ASSERTION_ALWAYS_IGNORE) {
        data->always_ignore = true;
        state = SDL_ASSERTION_IGNORE;
    }
    if (state == SDL_ASSERTION_ABORT) {
        abort();
    }
    return state;
}

void SDL_SetAssertionHandler(SDL_AssertionHandler handler, void *userdata)
{
    sdlop_assert_handler = handler;
    sdlop_assert_handler_userdata = userdata;
}

SDL_AssertionHandler SDL_GetDefaultAssertionHandler(void)
{
    return sdlop_default_assert_handler;
}

SDL_AssertionHandler SDL_GetAssertionHandler(void **puserdata)
{
    if (puserdata) {
        *puserdata = sdlop_assert_handler_userdata;
    }
    return sdlop_assert_handler ? sdlop_assert_handler : sdlop_default_assert_handler;
}

void SDL_ResetAssertionReport(void)
{
    /* SDLop does not keep an assertion report list. */
}
