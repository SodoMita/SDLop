/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_assert.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Assertions. The macro machinery is upstream's; SDLop implements
 * SDL_ReportAssertion(), the handler plumbing and SDL_ResetAssertionReport().
*/

#ifndef SDL_assert_h_
#define SDL_assert_h_

#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#define SDL_ASSERT_LEVEL SomeNumberBasedOnVariousFactors

#elif !defined(SDL_ASSERT_LEVEL)

#ifdef SDL_DEFAULT_ASSERT_LEVEL

#define SDL_ASSERT_LEVEL SDL_DEFAULT_ASSERT_LEVEL

#elif defined(_DEBUG) || defined(DEBUG) || \
 (defined(__GNUC__) && !defined(__OPTIMIZE__))

#define SDL_ASSERT_LEVEL 2

#else

#define SDL_ASSERT_LEVEL 1

#endif

#endif

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#elif defined(_MSC_VER) && _MSC_VER >= 1310

#elif defined(ANDROID)

#elif defined(HAVE_SIGNAL_H) && !defined(__WATCOMC__)

#include <signal.h>
 #define SDL_TriggerBreakpoint() raise(SIGTRAP)
#else

#endif

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#define SDL_FUNCTION __FUNCTION__

#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define SDL_FUNCTION __func__
#elif ((defined(__GNUC__) && (__GNUC__ >= 2)) || defined(_MSC_VER) || defined (__WATCOMC__))
# define SDL_FUNCTION __FUNCTION__
#else
# define SDL_FUNCTION "???"
#endif

#define SDL_FILE __FILE__

#define SDL_LINE __LINE__

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#define SDL_NULL_WHILE_LOOP_CONDITION (0)

#elif defined(_MSC_VER)

#define SDL_NULL_WHILE_LOOP_CONDITION (0,0)
#else
#define SDL_NULL_WHILE_LOOP_CONDITION (0)
#endif

#define SDL_disabled_assert(condition) \
 do { (void) sizeof ((condition)); } while (SDL_NULL_WHILE_LOOP_CONDITION)

typedef enum SDL_AssertState
{
 SDL_ASSERTION_RETRY,
 SDL_ASSERTION_BREAK,
 SDL_ASSERTION_ABORT,
 SDL_ASSERTION_IGNORE,
 SDL_ASSERTION_ALWAYS_IGNORE
} SDL_AssertState;

typedef struct SDL_AssertData
{
 bool always_ignore;
 unsigned int trigger_count;
 const char *condition;
 const char *filename;
 int linenum;
 const char *function;
 const struct SDL_AssertData *next;
} SDL_AssertData;

extern SDL_DECLSPEC SDL_AssertState SDLCALL SDL_ReportAssertion(SDL_AssertData *data,
 const char *func,
 const char *file, int line) SDL_ANALYZER_NORETURN;

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#define SDL_AssertBreakpoint() SDL_TriggerBreakpoint()

#elif !defined(SDL_AssertBreakpoint)

# if defined(ANDROID) && defined(assert)

# define SDL_AssertBreakpoint()

# else

# define SDL_AssertBreakpoint() SDL_TriggerBreakpoint()

# endif

#endif

#define SDL_enabled_assert(condition) \
 do { \
 while ( !(condition) ) { \
 static struct SDL_AssertData sdl_assert_data = { 0, 0, #condition, 0, 0, 0, 0 }; \
 const SDL_AssertState sdl_assert_state = SDL_ReportAssertion(&sdl_assert_data, SDL_FUNCTION, SDL_FILE, SDL_LINE); \
 if (sdl_assert_state == SDL_ASSERTION_RETRY) { \
 continue; \
 } else if (sdl_assert_state == SDL_ASSERTION_BREAK) { \
 SDL_AssertBreakpoint(); \
 } \
 break; \
 } \
 } while (SDL_NULL_WHILE_LOOP_CONDITION)

#ifdef SDL_WIKI_DOCUMENTATION_SECTION

#define SDL_assert(condition) if (assertion_enabled && (condition)) { trigger_assertion; }

#define SDL_assert_release(condition) SDL_disabled_assert(condition)

#define SDL_assert_paranoid(condition) SDL_disabled_assert(condition)

#elif SDL_ASSERT_LEVEL == 0

# define SDL_assert(condition) SDL_disabled_assert(condition)

# define SDL_assert_release(condition) SDL_disabled_assert(condition)

# define SDL_assert_paranoid(condition) SDL_disabled_assert(condition)

#elif SDL_ASSERT_LEVEL == 1

# define SDL_assert(condition) SDL_disabled_assert(condition)

# define SDL_assert_release(condition) SDL_enabled_assert(condition)

# define SDL_assert_paranoid(condition) SDL_disabled_assert(condition)

#elif SDL_ASSERT_LEVEL == 2

# define SDL_assert(condition) SDL_enabled_assert(condition)

# define SDL_assert_release(condition) SDL_enabled_assert(condition)

# define SDL_assert_paranoid(condition) SDL_disabled_assert(condition)

#elif SDL_ASSERT_LEVEL == 3

# define SDL_assert(condition) SDL_enabled_assert(condition)

# define SDL_assert_release(condition) SDL_enabled_assert(condition)

# define SDL_assert_paranoid(condition) SDL_enabled_assert(condition)

#else

#endif

#define SDL_assert_always(condition) SDL_enabled_assert(condition)

typedef SDL_AssertState (SDLCALL *SDL_AssertionHandler)(
 const SDL_AssertData *data, void *userdata);

extern SDL_DECLSPEC void SDLCALL SDL_SetAssertionHandler(
 SDL_AssertionHandler handler,
 void *userdata);

extern SDL_DECLSPEC SDL_AssertionHandler SDLCALL SDL_GetDefaultAssertionHandler(void);

extern SDL_DECLSPEC SDL_AssertionHandler SDLCALL SDL_GetAssertionHandler(void **puserdata);

extern SDL_DECLSPEC void SDLCALL SDL_ResetAssertionReport(void);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_assert_h_ */
