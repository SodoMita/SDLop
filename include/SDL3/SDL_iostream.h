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

/* WIKI CATEGORY: IOStream */

/**
 * # CategoryIOStream
 *
 * SDL provides an abstract interface for reading and writing data streams. It
 * offers implementations for files, memory, etc, and the app can provide
 * their own implementations, too.
 *
 * SDL_IOStream is not related to the standard C++ iostream class, other than
 * both are abstract interfaces to read/write data.
 */

#ifndef SDL_iostream_h_
#define SDL_iostream_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * SDL_IOStream status, set by a read or write operation.
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_IOStatus
{
    SDL_IO_STATUS_READY,     /**< Everything is ready (no errors and not EOF). */
    SDL_IO_STATUS_ERROR,     /**< Read or write I/O error */
    SDL_IO_STATUS_EOF,       /**< End of file */
    SDL_IO_STATUS_NOT_READY, /**< Non blocking I/O, not ready */
    SDL_IO_STATUS_READONLY,  /**< Tried to write a read-only buffer */
    SDL_IO_STATUS_WRITEONLY  /**< Tried to read a write-only buffer */
} SDL_IOStatus;

/**
 * Possible `whence` values for SDL_IOStream seeking.
 *
 * These map to the same "whence" concept that `fseek` or `lseek` use in the
 * standard C runtime.
 *
 * \since This enum is available since SDL 3.2.0.
 */
typedef enum SDL_IOWhence
{
    SDL_IO_SEEK_SET,  /**< Seek from the beginning of data */
    SDL_IO_SEEK_CUR,  /**< Seek relative to current read point */
    SDL_IO_SEEK_END   /**< Seek relative to the end of data */
} SDL_IOWhence;

/**
 * The function pointers that drive an SDL_IOStream.
 *
 * Applications can provide this struct to SDL_OpenIO() to create their own
 * implementation of SDL_IOStream. This is not necessarily required, as SDL
 * already offers several common types of I/O streams, via functions like
 * SDL_IOFromFile() and SDL_IOFromMem().
 *
 * This structure should be initialized using SDL_INIT_INTERFACE()
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_INIT_INTERFACE
 */
typedef struct SDL_IOStreamInterface
{
    /* The version of this interface */
    Uint32 version;

    /**
     *  Return the number of bytes in this SDL_IOStream
     *
     *  \return the total size of the data stream, or -1 on error.
     */
    Sint64 (SDLCALL *size)(void *userdata);

    /**
     *  Seek to `offset` relative to `whence`, one of stdio's whence values:
     *  SDL_IO_SEEK_SET, SDL_IO_SEEK_CUR, SDL_IO_SEEK_END
     *
     *  \return the final offset in the data stream, or -1 on error.
     */
    Sint64 (SDLCALL *seek)(void *userdata, Sint64 offset, SDL_IOWhence whence);

    /**
     *  Read up to `size` bytes from the data stream to the area pointed
     *  at by `ptr`. `size` will always be > 0.
     *
     *  On an incomplete read, you should set `*status` to a value from the
     *  SDL_IOStatus enum. You do not have to explicitly set this on
     *  a complete, successful read.
     *
     *  \return the number of bytes read
     */
    size_t (SDLCALL *read)(void *userdata, void *ptr, size_t size, SDL_IOStatus *status);

    /**
     *  Write exactly `size` bytes from the area pointed at by `ptr`
     *  to data stream. `size` will always be > 0.
     *
     *  On an incomplete write, you should set `*status` to a value from the
     *  SDL_IOStatus enum. You do not have to explicitly set this on
     *  a complete, successful write.
     *
     *  \return the number of bytes written
     */
    size_t (SDLCALL *write)(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status);

    /**
     *  If the stream is buffering, make sure the data is written out.
     *
     *  On failure, you should set `*status` to a value from the
     *  SDL_IOStatus enum. You do not have to explicitly set this on
     *  a successful flush.
     *
     *  \return true if successful or false on write error when flushing data.
     */
    bool (SDLCALL *flush)(void *userdata, SDL_IOStatus *status);

    /**
     *  Close and free any allocated resources.
     *
     *  This does not guarantee file writes will sync to physical media; they
     *  can be in the system's file cache, waiting to go to disk.
     *
     *  The SDL_IOStream is still destroyed even if this fails, so clean up anything
     *  even if flushing buffers, etc, returns an error.
     *
     *  \return true if successful or false on write error when flushing data.
     */
    bool (SDLCALL *close)(void *userdata);

} SDL_IOStreamInterface;

/* Check the size of SDL_IOStreamInterface
 *
 * If this assert fails, either the compiler is padding to an unexpected size,
 * or the interface has been updated and this should be updated to match and
 * the code using this interface should be updated to handle the old version.
 */
SDL_COMPILE_TIME_ASSERT(SDL_IOStreamInterface_SIZE,
    (sizeof(void *) == 4 && sizeof(SDL_IOStreamInterface) == 28) ||
    (sizeof(void *) == 8 && sizeof(SDL_IOStreamInterface) == 56));

/**
 * The read/write operation structure.
 *
 * This operates as an opaque handle. There are several APIs to create various
 * types of I/O streams, or an app can supply an SDL_IOStreamInterface to
 * SDL_OpenIO() to provide their own stream implementation behind this
 * struct's abstract interface.
 *
 * \since This struct is available since SDL 3.2.0.
 */
typedef struct SDL_IOStream SDL_IOStream;


/**
 *  \name IOFrom functions
 *
 *  Functions to create SDL_IOStream structures from various data streams.
 */
/* @{ */


#define SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER    "SDL.iostream.windows.handle"
#define SDL_PROP_IOSTREAM_STDIO_FILE_POINTER        "SDL.iostream.stdio.file"
#define SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER    "SDL.iostream.file_descriptor"
#define SDL_PROP_IOSTREAM_ANDROID_AASSET_POINTER    "SDL.iostream.android.aasset"


#define SDL_PROP_IOSTREAM_MEMORY_POINTER "SDL.iostream.memory.base"
#define SDL_PROP_IOSTREAM_MEMORY_SIZE_NUMBER  "SDL.iostream.memory.size"
#define SDL_PROP_IOSTREAM_MEMORY_FREE_FUNC_POINTER "SDL.iostream.memory.free"



#define SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER    "SDL.iostream.dynamic.memory"
#define SDL_PROP_IOSTREAM_DYNAMIC_CHUNKSIZE_NUMBER  "SDL.iostream.dynamic.chunksize"

/* @} *//* IOFrom functions */


















/**
 *  \name Read endian functions
 *
 *  Read an item of the specified endianness and return in native format.
 */
/* @{ */














/* @} *//* Read endian functions */

/**
 *  \name Write endian functions
 *
 *  Write an item of native format to the specified endianness.
 */
/* @{ */















/* @} *//* Write endian functions */

/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_iostream_h_ */
