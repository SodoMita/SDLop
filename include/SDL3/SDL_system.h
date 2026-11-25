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
 * # CategorySystem
 *
 * Platform-specific SDL API functions. These are functions that deal with
 * needs of specific operating systems, that didn't make sense to offer as
 * platform-independent, generic APIs.
 *
 * Most apps can make do without these functions, but they can be useful for
 * integrating with other parts of a specific system, adding platform-specific
 * polish to an app, or solving problems that only affect one target.
 */

#ifndef SDL_system_h_
#define SDL_system_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif


/*
 * Platform specific functions for Windows
 */
#if defined(SDL_PLATFORM_WINDOWS)

typedef struct tagMSG MSG;

/**
 * A callback to be used with SDL_SetWindowsMessageHook.
 *
 * This callback may modify the message, and should return true if the message
 * should continue to be processed, or false to prevent further processing.
 *
 * As this is processing a message directly from the Windows event loop, this
 * callback should do the minimum required work and return quickly.
 *
 * \param userdata the app-defined pointer provided to
 *                 SDL_SetWindowsMessageHook.
 * \param msg a pointer to a Win32 event structure to process.
 * \returns true to let event continue on, false to drop it.
 *
 * \threadsafety This may only be called (by SDL) from the thread handling the
 *               Windows event loop.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_SetWindowsMessageHook
 * \sa SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP
 */
typedef bool (SDLCALL *SDL_WindowsMessageHook)(void *userdata, MSG *msg);


#endif /* defined(SDL_PLATFORM_WINDOWS) */

#if defined(SDL_PLATFORM_WIN32) || defined(SDL_PLATFORM_WINGDK)



#endif /* defined(SDL_PLATFORM_WIN32) || defined(SDL_PLATFORM_WINGDK) */


/*
 * Platform specific functions for UNIX
 */

/* this is defined in Xlib's headers, just need a simple declaration here. */
typedef union _XEvent XEvent;

/**
 * A callback to be used with SDL_SetX11EventHook.
 *
 * This callback may modify the event, and should return true if the event
 * should continue to be processed, or false to prevent further processing.
 *
 * As this is processing an event directly from the X11 event loop, this
 * callback should do the minimum required work and return quickly.
 *
 * \param userdata the app-defined pointer provided to SDL_SetX11EventHook.
 * \param xevent a pointer to an Xlib XEvent union to process.
 * \returns true to let event continue on, false to drop it.
 *
 * \threadsafety This may only be called (by SDL) from the thread handling the
 *               X11 event loop.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_SetX11EventHook
 */
typedef bool (SDLCALL *SDL_X11EventHook)(void *userdata, XEvent *xevent);


/* Platform specific functions for Linux*/
/* SDLop: the Windows build also exports these two (as the honest "only
 * available on Linux" error) because tools/api_exports_check.sh compares the
 * declared header set against the DLL and cannot evaluate this #ifdef. They
 * are still NOT declared to Windows applications, like stock SDL3. */
#if defined(SDL_PLATFORM_LINUX) || (defined(SDL_PLATFORM_WINDOWS) && defined(SDLOP_EXPORT_LINUX_API))

/**
 * Sets the UNIX nice value for a thread.
 *
 * This uses setpriority() if possible, and RealtimeKit if available.
 *
 * \param threadID the Unix thread ID to change priority of.
 * \param priority the new, Unix-specific, priority value.
 * \returns true on success or false on failure; call SDL_GetError() for more
 *          information.
 *
 * \threadsafety It is safe to call this function from any thread.
 *
 * \since This function is available since SDL 3.2.0.
 */
extern SDL_DECLSPEC bool SDLCALL SDL_SetLinuxThreadPriority(Sint64 threadID, int priority);

/**
 * Sets the priority (not nice level) and scheduling policy for a thread.
 *
 * This uses setpriority() if possible, and RealtimeKit if available.
 *
 * \param threadID the Unix thread ID to change priority of.
 * \param sdlPriority the new SDL_ThreadPriority value.
 * \param schedPolicy the new scheduling policy (SCHED_FIFO, SCHED_RR,
 *                    SCHED_OTHER, etc...).
 * \returns true on success or false on failure; call SDL_GetError() for more
 *          information.
 *
 * \threadsafety It is safe to call this function from any thread.
 *
 * \since This function is available since SDL 3.2.0.
 */
extern SDL_DECLSPEC bool SDLCALL SDL_SetLinuxThreadPriorityAndPolicy(Sint64 threadID, int sdlPriority, int schedPolicy);

#endif /* SDL_PLATFORM_LINUX */

/*
 * Platform specific functions for iOS
 */
#ifdef SDL_PLATFORM_IOS

/**
 * The prototype for an Apple iOS animation callback.
 *
 * This datatype is only useful on Apple iOS.
 *
 * After passing a function pointer of this type to
 * SDL_SetiOSAnimationCallback, the system will call that function pointer at
 * a regular interval.
 *
 * \param userdata what was passed as `callbackParam` to
 *                 SDL_SetiOSAnimationCallback as `callbackParam`.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_SetiOSAnimationCallback
 */
typedef void (SDLCALL *SDL_iOSAnimationCallback)(void *userdata);



#endif /* SDL_PLATFORM_IOS */


/*
 * Platform specific functions for Android
 */
#ifdef SDL_PLATFORM_ANDROID







/**
 * See the official Android developer guide for more information:
 * http://developer.android.com/guide/topics/data/data-storage.html
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_ANDROID_EXTERNAL_STORAGE_READ   0x01

/**
 * See the official Android developer guide for more information:
 * http://developer.android.com/guide/topics/data/data-storage.html
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_ANDROID_EXTERNAL_STORAGE_WRITE  0x02





/**
 * Callback that presents a response from a SDL_RequestAndroidPermission call.
 *
 * \param userdata an app-controlled pointer that is passed to the callback.
 * \param permission the Android-specific permission name that was requested.
 * \param granted true if permission is granted, false if denied.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_RequestAndroidPermission
 */
typedef void (SDLCALL *SDL_RequestAndroidPermissionCallback)(void *userdata, const char *permission, bool granted);




#endif /* SDL_PLATFORM_ANDROID */



/**
 * Application sandbox environment.
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_Sandbox
{
    SDL_SANDBOX_NONE = 0,
    SDL_SANDBOX_UNKNOWN_CONTAINER,
    SDL_SANDBOX_FLATPAK,
    SDL_SANDBOX_SNAP,
    SDL_SANDBOX_MACOS
} SDL_Sandbox;



/* Functions used by iOS app delegates to notify SDL about state changes. */







#ifdef SDL_PLATFORM_IOS

#endif

/*
 * Functions used only by GDK
 */
#ifdef SDL_PLATFORM_GDK
typedef struct XTaskQueueObject *XTaskQueueHandle;
typedef struct XUser *XUserHandle;



#endif

/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_system_h_ */
