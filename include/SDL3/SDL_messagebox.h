/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
 *
 * SDLop: generated from SDL 3.4.20 by tools/gen_headers.py - declarations of
 * functions SDLop does not implement are removed; everything else is
 * upstream's text. Do not edit by hand.
 */

/**
 * # CategoryMessagebox
 *
 * SDL offers a simple message box API, which is useful for simple alerts,
 * such as informing the user when something fatal happens at startup without
 * the need to build a UI for it (or informing the user _before_ your UI is
 * ready).
 *
 * These message boxes are native system dialogs where possible.
 *
 * There is both a customizable function (SDL_ShowMessageBox()) that offers
 * lots of options for what to display and reports on what choice the user
 * made, and also a much-simplified version (SDL_ShowSimpleMessageBox()),
 * merely takes a text message and title, and waits until the user presses a
 * single "OK" UI button. Often, this is all that is necessary.
 */

#ifndef SDL_messagebox_h_
#define SDL_messagebox_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>      /* For SDL_Window */

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message box flags.
 *
 * If supported will display warning icon, etc.
 *
 * \since This datatype is available since SDL 3.2.0.
 */
typedef Uint32 SDL_MessageBoxFlags;

#define SDL_MESSAGEBOX_ERROR                    0x00000010u /**< error dialog */
#define SDL_MESSAGEBOX_WARNING                  0x00000020u /**< warning dialog */
#define SDL_MESSAGEBOX_INFORMATION              0x00000040u /**< informational dialog */
#define SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT    0x00000080u /**< buttons placed left to right */
#define SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT    0x00000100u /**< buttons placed right to left */

/**
 * SDL_MessageBoxButtonData flags.
 *
 * \since This datatype is available since SDL 3.2.0.
 */
typedef Uint32 SDL_MessageBoxButtonFlags;

#define SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT 0x00000001u /**< Marks the default button when return is hit */
#define SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT 0x00000002u /**< Marks the default button when escape is hit */

/**
 * Individual button data.
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_MessageBoxButtonData
{
    SDL_MessageBoxButtonFlags flags;
    int buttonID;       /**< User defined button id (value returned via SDL_ShowMessageBox) */
    const char *text;   /**< The UTF-8 button text */
} SDL_MessageBoxButtonData;

/**
 * RGB value used in a message box color scheme
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_MessageBoxColor
{
    Uint8 r, g, b;
} SDL_MessageBoxColor;

/**
 * An enumeration of indices inside the colors array of
 * SDL_MessageBoxColorScheme.
 */
typedef enum SDL_MessageBoxColorType
{
    SDL_MESSAGEBOX_COLOR_BACKGROUND,
    SDL_MESSAGEBOX_COLOR_TEXT,
    SDL_MESSAGEBOX_COLOR_BUTTON_BORDER,
    SDL_MESSAGEBOX_COLOR_BUTTON_BACKGROUND,
    SDL_MESSAGEBOX_COLOR_BUTTON_SELECTED,
    SDL_MESSAGEBOX_COLOR_COUNT                    /**< Size of the colors array of SDL_MessageBoxColorScheme. */
} SDL_MessageBoxColorType;

/**
 * A set of colors to use for message box dialogs
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_MessageBoxColorScheme
{
    SDL_MessageBoxColor colors[SDL_MESSAGEBOX_COLOR_COUNT];
} SDL_MessageBoxColorScheme;

/**
 * MessageBox structure containing title, text, window, etc.
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_MessageBoxData
{
    SDL_MessageBoxFlags flags;
    SDL_Window *window;                 /**< Parent window, can be NULL */
    const char *title;                  /**< UTF-8 title */
    const char *message;                /**< UTF-8 message text */

    int numbuttons;
    const SDL_MessageBoxButtonData *buttons;

    const SDL_MessageBoxColorScheme *colorScheme;   /**< SDL_MessageBoxColorScheme, can be NULL to use system settings */
} SDL_MessageBoxData;




/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_messagebox_h_ */
