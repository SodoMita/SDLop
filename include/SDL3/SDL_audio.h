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
 * # CategoryAudio
 *
 * Audio functionality for the SDL library.
 *
 * All audio in SDL3 revolves around SDL_AudioStream. Whether you want to play
 * or record audio, convert it, stream it, buffer it, or mix it, you're going
 * to be passing it through an audio stream.
 *
 * Audio streams are quite flexible; they can accept any amount of data at a
 * time, in any supported format, and output it as needed in any other format,
 * even if the data format changes on either side halfway through.
 *
 * An app opens an audio device and binds any number of audio streams to it,
 * feeding more data to the streams as available. When the device needs more
 * data, it will pull it from all bound streams and mix them together for
 * playback.
 *
 * Audio streams can also use an app-provided callback to supply data
 * on-demand, which maps pretty closely to the SDL2 audio model.
 *
 * SDL also provides a simple .WAV loader in SDL_LoadWAV (and SDL_LoadWAV_IO
 * if you aren't reading from a file) as a basic means to load sound data into
 * your program.
 *
 * ## Logical audio devices
 *
 * In SDL3, opening a physical device (like a SoundBlaster 16 Pro) gives you a
 * logical device ID that you can bind audio streams to. In almost all cases,
 * logical devices can be used anywhere in the API that a physical device is
 * normally used. However, since each device opening generates a new logical
 * device, different parts of the program (say, a VoIP library, or
 * text-to-speech framework, or maybe some other sort of mixer on top of SDL)
 * can have their own device opens that do not interfere with each other; each
 * logical device will mix its separate audio down to a single buffer, fed to
 * the physical device, behind the scenes. As many logical devices as you like
 * can come and go; SDL will only have to open the physical device at the OS
 * level once, and will manage all the logical devices on top of it
 * internally.
 *
 * One other benefit of logical devices: if you don't open a specific physical
 * device, instead opting for the default, SDL can automatically migrate those
 * logical devices to different hardware as circumstances change: a user
 * plugged in headphones? The system default changed? SDL can transparently
 * migrate the logical devices to the correct physical device seamlessly and
 * keep playing; the app doesn't even have to know it happened if it doesn't
 * want to.
 *
 * ## Simplified audio
 *
 * As a simplified model for when a single source of audio is all that's
 * needed, an app can use SDL_OpenAudioDeviceStream, which is a single
 * function to open an audio device, create an audio stream, bind that stream
 * to the newly-opened device, and (optionally) provide a callback for
 * obtaining audio data. When using this function, the primary interface is
 * the SDL_AudioStream and the device handle is mostly hidden away; destroying
 * a stream created through this function will also close the device, stream
 * bindings cannot be changed, etc. One other quirk of this is that the device
 * is started in a _paused_ state and must be explicitly resumed; this is
 * partially to offer a clean migration for SDL2 apps and partially because
 * the app might have to do more setup before playback begins; in the
 * non-simplified form, nothing will play until a stream is bound to a device,
 * so they start _unpaused_.
 *
 * ## Channel layouts
 *
 * Audio data passing through SDL is uncompressed PCM data, interleaved. One
 * can provide their own decompression through an MP3, etc, decoder, but SDL
 * does not provide this directly. Each interleaved channel of data is meant
 * to be in a specific order.
 *
 * Abbreviations:
 *
 * - FRONT = single mono speaker
 * - FL = front left speaker
 * - FR = front right speaker
 * - FC = front center speaker
 * - BL = back left speaker
 * - BR = back right speaker
 * - SR = surround right speaker
 * - SL = surround left speaker
 * - BC = back center speaker
 * - LFE = low-frequency speaker
 *
 * These are listed in the order they are laid out in memory, so "FL, FR"
 * means "the front left speaker is laid out in memory first, then the front
 * right, then it repeats for the next audio frame".
 *
 * - 1 channel (mono) layout: FRONT
 * - 2 channels (stereo) layout: FL, FR
 * - 3 channels (2.1) layout: FL, FR, LFE
 * - 4 channels (quad) layout: FL, FR, BL, BR
 * - 5 channels (4.1) layout: FL, FR, LFE, BL, BR
 * - 6 channels (5.1) layout: FL, FR, FC, LFE, BL, BR (last two can also be
 *   SL, SR)
 * - 7 channels (6.1) layout: FL, FR, FC, LFE, BC, SL, SR
 * - 8 channels (7.1) layout: FL, FR, FC, LFE, BL, BR, SL, SR
 *
 * This is the same order as DirectSound expects, but applied to all
 * platforms; SDL will swizzle the channels as necessary if a platform expects
 * something different.
 *
 * SDL_AudioStream can also be provided channel maps to change this ordering
 * to whatever is necessary, in other audio processing scenarios.
 */

#ifndef SDL_audio_h_
#define SDL_audio_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_endian.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_iostream.h>

#include <SDL3/SDL_begin_code.h>
/* Set up for C function definitions, even when using C++ */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mask of bits in an SDL_AudioFormat that contains the format bit size.
 *
 * Generally one should use SDL_AUDIO_BITSIZE instead of this macro directly.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_MASK_BITSIZE       (0xFFu)

/**
 * Mask of bits in an SDL_AudioFormat that contain the floating point flag.
 *
 * Generally one should use SDL_AUDIO_ISFLOAT instead of this macro directly.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_MASK_FLOAT         (1u<<8)

/**
 * Mask of bits in an SDL_AudioFormat that contain the bigendian flag.
 *
 * Generally one should use SDL_AUDIO_ISBIGENDIAN or SDL_AUDIO_ISLITTLEENDIAN
 * instead of this macro directly.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_MASK_BIG_ENDIAN    (1u<<12)

/**
 * Mask of bits in an SDL_AudioFormat that contain the signed data flag.
 *
 * Generally one should use SDL_AUDIO_ISSIGNED instead of this macro directly.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_MASK_SIGNED        (1u<<15)

/**
 * Define an SDL_AudioFormat value.
 *
 * SDL does not support custom audio formats, so this macro is not of much use
 * externally, but it can be illustrative as to what the various bits of an
 * SDL_AudioFormat mean.
 *
 * For example, SDL_AUDIO_S32LE looks like this:
 *
 * ```c
 * SDL_DEFINE_AUDIO_FORMAT(1, 0, 0, 32)
 * ```
 *
 * \param signed 1 for signed data, 0 for unsigned data.
 * \param bigendian 1 for bigendian data, 0 for littleendian data.
 * \param flt 1 for floating point data, 0 for integer data.
 * \param size number of bits per sample.
 * \returns a format value in the style of SDL_AudioFormat.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_DEFINE_AUDIO_FORMAT(signed, bigendian, flt, size) \
    (((Uint16)(signed) << 15) | ((Uint16)(bigendian) << 12) | ((Uint16)(flt) << 8) | ((size) & SDL_AUDIO_MASK_BITSIZE))

/**
 * Audio format.
 *
 * \since This enum is available since SDL 3.2.0.
 *
 * \sa SDL_AUDIO_BITSIZE
 * \sa SDL_AUDIO_BYTESIZE
 * \sa SDL_AUDIO_ISINT
 * \sa SDL_AUDIO_ISFLOAT
 * \sa SDL_AUDIO_ISBIGENDIAN
 * \sa SDL_AUDIO_ISLITTLEENDIAN
 * \sa SDL_AUDIO_ISSIGNED
 * \sa SDL_AUDIO_ISUNSIGNED
 */
typedef enum SDL_AudioFormat
{
    SDL_AUDIO_UNKNOWN   = 0x0000u,  /**< Unspecified audio format */
    SDL_AUDIO_U8        = 0x0008u,  /**< Unsigned 8-bit samples */
        /* SDL_DEFINE_AUDIO_FORMAT(0, 0, 0, 8), */
    SDL_AUDIO_S8        = 0x8008u,  /**< Signed 8-bit samples */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 0, 0, 8), */
    SDL_AUDIO_S16LE     = 0x8010u,  /**< Signed 16-bit samples */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 0, 0, 16), */
    SDL_AUDIO_S16BE     = 0x9010u,  /**< As above, but big-endian byte order */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 1, 0, 16), */
    SDL_AUDIO_S32LE     = 0x8020u,  /**< 32-bit integer samples */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 0, 0, 32), */
    SDL_AUDIO_S32BE     = 0x9020u,  /**< As above, but big-endian byte order */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 1, 0, 32), */
    SDL_AUDIO_F32LE     = 0x8120u,  /**< 32-bit floating point samples */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 0, 1, 32), */
    SDL_AUDIO_F32BE     = 0x9120u,  /**< As above, but big-endian byte order */
        /* SDL_DEFINE_AUDIO_FORMAT(1, 1, 1, 32), */

    /* These represent the current system's byteorder. */
    #if SDL_BYTEORDER == SDL_LIL_ENDIAN
    SDL_AUDIO_S16 = SDL_AUDIO_S16LE,
    SDL_AUDIO_S32 = SDL_AUDIO_S32LE,
    SDL_AUDIO_F32 = SDL_AUDIO_F32LE
    #else
    SDL_AUDIO_S16 = SDL_AUDIO_S16BE,
    SDL_AUDIO_S32 = SDL_AUDIO_S32BE,
    SDL_AUDIO_F32 = SDL_AUDIO_F32BE
    #endif
} SDL_AudioFormat;


/**
 * Retrieve the size, in bits, from an SDL_AudioFormat.
 *
 * For example, `SDL_AUDIO_BITSIZE(SDL_AUDIO_S16)` returns 16.
 *
 * \param x an SDL_AudioFormat value.
 * \returns data size in bits.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_BITSIZE(x)         ((x) & SDL_AUDIO_MASK_BITSIZE)

/**
 * Retrieve the size, in bytes, from an SDL_AudioFormat.
 *
 * For example, `SDL_AUDIO_BYTESIZE(SDL_AUDIO_S16)` returns 2.
 *
 * \param x an SDL_AudioFormat value.
 * \returns data size in bytes.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_BYTESIZE(x)        (SDL_AUDIO_BITSIZE(x) / 8)

/**
 * Determine if an SDL_AudioFormat represents floating point data.
 *
 * For example, `SDL_AUDIO_ISFLOAT(SDL_AUDIO_S16)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is floating point, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISFLOAT(x)         ((x) & SDL_AUDIO_MASK_FLOAT)

/**
 * Determine if an SDL_AudioFormat represents bigendian data.
 *
 * For example, `SDL_AUDIO_ISBIGENDIAN(SDL_AUDIO_S16LE)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is bigendian, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISBIGENDIAN(x)     ((x) & SDL_AUDIO_MASK_BIG_ENDIAN)

/**
 * Determine if an SDL_AudioFormat represents littleendian data.
 *
 * For example, `SDL_AUDIO_ISLITTLEENDIAN(SDL_AUDIO_S16BE)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is littleendian, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISLITTLEENDIAN(x)  (!SDL_AUDIO_ISBIGENDIAN(x))

/**
 * Determine if an SDL_AudioFormat represents signed data.
 *
 * For example, `SDL_AUDIO_ISSIGNED(SDL_AUDIO_U8)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is signed, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISSIGNED(x)        ((x) & SDL_AUDIO_MASK_SIGNED)

/**
 * Determine if an SDL_AudioFormat represents integer data.
 *
 * For example, `SDL_AUDIO_ISINT(SDL_AUDIO_F32)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is integer, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISINT(x)           (!SDL_AUDIO_ISFLOAT(x))

/**
 * Determine if an SDL_AudioFormat represents unsigned data.
 *
 * For example, `SDL_AUDIO_ISUNSIGNED(SDL_AUDIO_S16)` returns 0.
 *
 * \param x an SDL_AudioFormat value.
 * \returns non-zero if format is unsigned, zero otherwise.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_ISUNSIGNED(x)      (!SDL_AUDIO_ISSIGNED(x))


/**
 * SDL Audio Device instance IDs.
 *
 * Zero is used to signify an invalid/null device.
 *
 * \since This datatype is available since SDL 3.2.0.
 */
typedef Uint32 SDL_AudioDeviceID;

/**
 * A value used to request a default playback audio device.
 *
 * Several functions that require an SDL_AudioDeviceID will accept this value
 * to signify the app just wants the system to choose a default device instead
 * of the app providing a specific one.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID) 0xFFFFFFFFu)

/**
 * A value used to request a default recording audio device.
 *
 * Several functions that require an SDL_AudioDeviceID will accept this value
 * to signify the app just wants the system to choose a default device instead
 * of the app providing a specific one.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_DEVICE_DEFAULT_RECORDING ((SDL_AudioDeviceID) 0xFFFFFFFEu)

/**
 * Format specifier for audio data.
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_AudioFormat
 */
typedef struct SDL_AudioSpec
{
    SDL_AudioFormat format;     /**< Audio data format */
    int channels;               /**< Number of channels: 1 mono, 2 stereo, etc */
    int freq;                   /**< sample rate: sample frames per second */
} SDL_AudioSpec;

/**
 * Calculate the size of each audio frame (in bytes) from an SDL_AudioSpec.
 *
 * This reports on the size of an audio sample frame: stereo Sint16 data (2
 * channels of 2 bytes each) would be 4 bytes per frame, for example.
 *
 * This casts the value to unsigned int to avoid implicit sign conversion.
 *
 * \param x an SDL_AudioSpec to query.
 * \returns the number of bytes used per sample frame.
 *
 * \threadsafety It is safe to call this macro from any thread.
 *
 * \since This macro is available since SDL 3.2.0.
 */
#define SDL_AUDIO_FRAMESIZE(x) (((unsigned int)SDL_AUDIO_BYTESIZE((x).format)) * ((unsigned int)((x).channels)))

/**
 * The opaque handle that represents an audio stream.
 *
 * SDL_AudioStream is an audio conversion interface.
 *
 * - It can handle resampling data in chunks without generating artifacts,
 *   when it doesn't have the complete buffer available.
 * - It can handle incoming data in any variable size.
 * - It can handle input/output format changes on the fly.
 * - It can remap audio channels between inputs and outputs.
 * - You push data as you have it, and pull it when you need it; the
 *   stream will buffer data as needed.
 * - It can also function as a basic audio data queue even if you just have
 *   sound that needs to pass from one place to another.
 * - You can hook callbacks up to them when more data is added or requested,
 *   to manage data on-the-fly.
 *
 * Audio streams are the core of the SDL3 audio interface. You create one or
 * more of them, bind them to an opened audio device, and feed data to them
 * (or for recording, consume data from them).
 *
 * \since This struct is available since SDL 3.2.0.
 *
 * \sa SDL_CreateAudioStream
 */
typedef struct SDL_AudioStream SDL_AudioStream;


/* Function prototypes */

























#define SDL_PROP_AUDIOSTREAM_AUTO_CLEANUP_BOOLEAN "SDL.audiostream.auto_cleanup"













/**
 * A callback that fires for completed SDL_PutAudioStreamDataNoCopy() data.
 *
 * When using SDL_PutAudioStreamDataNoCopy() to provide data to an
 * SDL_AudioStream, it's not safe to dispose of the data until the stream has
 * completely consumed it. Often times it's difficult to know exactly when
 * this has happened.
 *
 * This callback fires once when the stream no longer needs the buffer,
 * allowing the app to easily free or reuse it.
 *
 * \param userdata an opaque pointer provided by the app for their personal
 *                 use.
 * \param buf the pointer provided to SDL_PutAudioStreamDataNoCopy().
 * \param buflen the size of buffer, in bytes, provided to
 *               SDL_PutAudioStreamDataNoCopy().
 *
 * \threadsafety This callbacks may run from any thread, so if you need to
 *               protect shared data, you should use SDL_LockAudioStream to
 *               serialize access; this lock will be held before your callback
 *               is called, so your callback does not need to manage the lock
 *               explicitly.
 *
 * \since This datatype is available since SDL 3.4.0.
 *
 * \sa SDL_SetAudioStreamGetCallback
 * \sa SDL_SetAudioStreamPutCallback
 */
typedef void (SDLCALL *SDL_AudioStreamDataCompleteCallback)(void *userdata, const void *buf, int buflen);

















/**
 * A callback that fires when data passes through an SDL_AudioStream.
 *
 * Apps can (optionally) register a callback with an audio stream that is
 * called when data is added with SDL_PutAudioStreamData, or requested with
 * SDL_GetAudioStreamData.
 *
 * Two values are offered here: one is the amount of additional data needed to
 * satisfy the immediate request (which might be zero if the stream already
 * has enough data queued) and the other is the total amount being requested.
 * In a Get call triggering a Put callback, these values can be different. In
 * a Put call triggering a Get callback, these values are always the same.
 *
 * Byte counts might be slightly overestimated due to buffering or resampling,
 * and may change from call to call.
 *
 * This callback is not required to do anything. Generally this is useful for
 * adding/reading data on demand, and the app will often put/get data as
 * appropriate, but the system goes on with the data currently available to it
 * if this callback does nothing.
 *
 * Do not call SDL_DestroyAudioStream() on `stream` during this callback.
 *
 * \param stream the SDL audio stream associated with this callback.
 * \param additional_amount the amount of data, in bytes, that is needed right
 *                          now.
 * \param total_amount the total amount of data requested, in bytes, that is
 *                     requested or available.
 * \param userdata an opaque pointer provided by the app for their personal
 *                 use.
 *
 * \threadsafety This callbacks may run from any thread, so if you need to
 *               protect shared data, you should use SDL_LockAudioStream to
 *               serialize access; this lock will be held before your callback
 *               is called, so your callback does not need to manage the lock
 *               explicitly.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_SetAudioStreamGetCallback
 * \sa SDL_SetAudioStreamPutCallback
 */
typedef void (SDLCALL *SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);







/**
 * A callback that fires when data is about to be fed to an audio device.
 *
 * This is useful for accessing the final mix, perhaps for writing a
 * visualizer or applying a final effect to the audio data before playback.
 *
 * This callback should run as quickly as possible and not block for any
 * significant time, as this callback delays submission of data to the audio
 * device, which can cause audio playback problems.
 *
 * The postmix callback _must_ be able to handle any audio data format
 * specified in `spec`, which can change between callbacks if the audio device
 * changed. However, this only covers frequency and channel count; data is
 * always provided here in SDL_AUDIO_F32 format.
 *
 * The postmix callback runs _after_ logical device gain and audiostream gain
 * have been applied, which is to say you can make the output data louder at
 * this point than the gain settings would suggest.
 *
 * \param userdata a pointer provided by the app through
 *                 SDL_SetAudioPostmixCallback, for its own use.
 * \param spec the current format of audio that is to be submitted to the
 *             audio device.
 * \param buffer the buffer of audio samples to be submitted. The callback can
 *               inspect and/or modify this data.
 * \param buflen the size of `buffer` in bytes.
 *
 * \threadsafety This will run from a background thread owned by SDL. The
 *               application is responsible for locking resources the callback
 *               touches that need to be protected.
 *
 * \since This datatype is available since SDL 3.2.0.
 *
 * \sa SDL_SetAudioPostmixCallback
 */
typedef void (SDLCALL *SDL_AudioPostmixCallback)(void *userdata, const SDL_AudioSpec *spec, float *buffer, int buflen);










/* Ends C function definitions when using C++ */
#ifdef __cplusplus
}
#endif
#include <SDL3/SDL_close_code.h>

#endif /* SDL_audio_h_ */
