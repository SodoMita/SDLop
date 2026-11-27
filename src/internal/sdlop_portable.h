/*
  SDLop - zero-cost portable POSIX fallbacks for BSDs / Solaris / Haiku / QNX.

  On Linux every helper here expands at compile time to the direct Linux
  syscall (eventfd, pipe2, memfd_create) with zero extra instructions or
  allocations.  On non-Linux POSIX targets each expands to the minimal
  POSIX equivalent (self-pipe, pipe()+fcntl(), shm_open()+shm_unlink() /
  mkostemp(), etc.) matching SDL 3.4.20's behaviour.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_portable_h_
#define sdlop_portable_h_

#include <SDL3/SDL_stdinc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Platform detection                                                  */
/* ------------------------------------------------------------------ */

#if defined(SDL_PLATFORM_LINUX) || defined(__linux__) || defined(__linux)
#define SDLOP_PLATFORM_LINUX 1
#endif
#if defined(SDL_PLATFORM_FREEBSD) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || \
    defined(__DragonFly__)
#define SDLOP_PLATFORM_FREEBSD 1
#endif
#if defined(SDL_PLATFORM_NETBSD) || defined(__NetBSD__)
#define SDLOP_PLATFORM_NETBSD 1
#endif
#if defined(SDL_PLATFORM_OPENBSD) || defined(__OpenBSD__)
#define SDLOP_PLATFORM_OPENBSD 1
#endif
#if defined(SDLOP_PLATFORM_FREEBSD) || defined(SDLOP_PLATFORM_NETBSD) || \
    defined(SDLOP_PLATFORM_OPENBSD) || defined(SDL_PLATFORM_BSDI) || defined(__bsdi__)
#define SDLOP_PLATFORM_BSD 1
#endif
#if defined(SDL_PLATFORM_SOLARIS) || defined(__sun) || defined(__sun__)
#define SDLOP_PLATFORM_SOLARIS 1
#endif
#if defined(SDL_PLATFORM_HAIKU) || defined(__HAIKU__)
#define SDLOP_PLATFORM_HAIKU 1
#endif
#if defined(SDL_PLATFORM_QNXNTO) || defined(__QNX__) || defined(__QNXNTO__)
#define SDLOP_PLATFORM_QNX 1
#endif
#if defined(SDL_PLATFORM_APPLE) || defined(__APPLE__)
#define SDLOP_PLATFORM_APPLE 1
#endif

/* ------------------------------------------------------------------ */
/* Fallback feature macros when built outside CMake                    */
/* ------------------------------------------------------------------ */

#ifndef SDLOP_CMAKE_PROBED
#ifdef SDLOP_PLATFORM_LINUX
#ifndef SDLOP_HAVE_EVENTFD
#define SDLOP_HAVE_EVENTFD 1
#endif
#ifndef SDLOP_HAVE_PIPE2
#define SDLOP_HAVE_PIPE2 1
#endif
#ifndef SDLOP_HAVE_MEMFD_CREATE
#define SDLOP_HAVE_MEMFD_CREATE 1
#endif
#ifndef SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK
#define SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#endif
#ifndef SDLOP_HAVE_CLOCK_NANOSLEEP
#define SDLOP_HAVE_CLOCK_NANOSLEEP 1
#endif
#ifndef SDLOP_HAVE_ICONV
#define SDLOP_HAVE_ICONV 1
#endif
#ifndef SDLOP_HAVE_MKOSTEMP
#define SDLOP_HAVE_MKOSTEMP 1
#endif
#ifndef SDLOP_HAVE_POSIX_FALLOCATE
#define SDLOP_HAVE_POSIX_FALLOCATE 1
#endif
#ifndef SDLOP_HAVE_PTHREAD_SETNAME_NP
#define SDLOP_HAVE_PTHREAD_SETNAME_NP 1
#endif
#if !defined(SDLOP_HAVE_SEM_CLOCKWAIT) && !defined(SDLOP_HAVE_SEM_TIMEDWAIT)
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
#define SDLOP_HAVE_SEM_CLOCKWAIT 1
#else
#define SDLOP_HAVE_SEM_TIMEDWAIT 1
#endif
#endif
#else /* !SDLOP_PLATFORM_LINUX */
#if defined(SDLOP_PLATFORM_BSD) || defined(SDLOP_PLATFORM_SOLARIS) || defined(SDLOP_PLATFORM_QNX)
#ifndef SDLOP_HAVE_PIPE2
#define SDLOP_HAVE_PIPE2 1
#endif
#ifndef SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK
#define SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#endif
#ifndef SDLOP_HAVE_CLOCK_NANOSLEEP
#define SDLOP_HAVE_CLOCK_NANOSLEEP 1
#endif
#ifndef SDLOP_HAVE_SEM_TIMEDWAIT
#define SDLOP_HAVE_SEM_TIMEDWAIT 1
#endif
#endif
#if defined(SDLOP_PLATFORM_FREEBSD) || defined(SDLOP_PLATFORM_OPENBSD)
#ifndef SDLOP_HAVE_PTHREAD_NP_H
#define SDLOP_HAVE_PTHREAD_NP_H 1
#endif
#ifndef SDLOP_HAVE_PTHREAD_SET_NAME_NP
#define SDLOP_HAVE_PTHREAD_SET_NAME_NP 1
#endif
#endif
#if defined(SDLOP_PLATFORM_NETBSD) || defined(SDLOP_PLATFORM_APPLE)
#ifndef SDLOP_HAVE_PTHREAD_SETNAME_NP
#define SDLOP_HAVE_PTHREAD_SETNAME_NP 1
#endif
#endif
#if defined(SDLOP_PLATFORM_FREEBSD) || defined(SDLOP_PLATFORM_NETBSD) || defined(SDLOP_PLATFORM_SOLARIS)
#ifndef SDLOP_HAVE_POSIX_FALLOCATE
#define SDLOP_HAVE_POSIX_FALLOCATE 1
#endif
#endif
#if defined(SDLOP_PLATFORM_FREEBSD) && defined(__FreeBSD__) && __FreeBSD__ >= 13
#ifndef SDLOP_HAVE_MEMFD_CREATE
#define SDLOP_HAVE_MEMFD_CREATE 1
#endif
#ifndef SDLOP_HAVE_SEM_CLOCKWAIT_NP
#define SDLOP_HAVE_SEM_CLOCKWAIT_NP 1
#endif
#endif
#if defined(SDLOP_PLATFORM_BSD) || defined(SDLOP_PLATFORM_SOLARIS) || defined(SDLOP_PLATFORM_APPLE)
#ifndef SDLOP_HAVE_MKOSTEMP
#define SDLOP_HAVE_MKOSTEMP 1
#endif
#endif
#if defined(SDLOP_PLATFORM_HAIKU)
#ifndef SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK
#define SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#endif
#ifndef SDLOP_HAVE_SEM_TIMEDWAIT
#define SDLOP_HAVE_SEM_TIMEDWAIT 1
#endif
#endif
#if !defined(SDLOP_PLATFORM_HAIKU) && !defined(SDLOP_PLATFORM_APPLE)
#ifndef SDLOP_HAVE_ICONV
#define SDLOP_HAVE_ICONV 1
#endif
#endif
#if defined(SDLOP_PLATFORM_BSD) || defined(SDLOP_PLATFORM_SOLARIS) || \
    defined(SDLOP_PLATFORM_HAIKU) || defined(SDLOP_PLATFORM_QNX) || \
    defined(SDLOP_PLATFORM_APPLE)
#ifndef SDLOP_HAVE_SHM_OPEN
#define SDLOP_HAVE_SHM_OPEN 1
#endif
#endif
#endif /* SDLOP_PLATFORM_LINUX */
#endif /* !SDLOP_CMAKE_PROBED */

/* ------------------------------------------------------------------ */
/* 0-cost inline POSIX primitives                                      */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

#ifndef O_CLOEXEC
#define SDLOP_O_CLOEXEC 0
#else
#define SDLOP_O_CLOEXEC O_CLOEXEC
#endif

#ifdef SOCK_CLOEXEC
#define SDLOP_SOCK_CLOEXEC SOCK_CLOEXEC
#else
#define SDLOP_SOCK_CLOEXEC 0
#endif

static inline void SDLOP_EnsureCloexec(int fd)
{
#if (!defined(O_CLOEXEC) || !defined(SOCK_CLOEXEC)) && defined(FD_CLOEXEC)
    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && !(fl & FD_CLOEXEC)) {
            fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        }
    }
#else
    (void)fd;
#endif
}

/* Create a pipe with CLOEXEC and optional NONBLOCK on the read end (or both).
 * On Linux/BSD/Solaris/QNX this compiles directly to pipe2(). */
static inline int SDLOP_PipeCreate(int fds[2], bool cloexec, bool nonblock_read)
{
#if defined(SDLOP_HAVE_PIPE2) && SDLOP_HAVE_PIPE2
    int flags = 0;
#ifdef O_CLOEXEC
    if (cloexec) {
        flags |= O_CLOEXEC;
    }
#endif
    if (nonblock_read) {
        flags |= O_NONBLOCK;
    }
    return pipe2(fds, flags);
#else
    if (pipe(fds) != 0) {
        return -1;
    }
    if (cloexec) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(fds[i], F_GETFD);
            if (fl >= 0) {
                fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC);
            }
        }
    }
    if (nonblock_read) {
        int fl = fcntl(fds[0], F_GETFL);
        if (fl >= 0) {
            fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
        }
    }
    return 0;
#endif
}

/* 0-cost wake fd: eventfd on Linux, self-pipe elsewhere. */
static inline void SDLOP_WakeFdCreate(int *rfd, int *wfd)
{
#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
    if (wfd) {
        *wfd = -1;
    }
    *rfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (*rfd < 0) {
        *rfd = -1;
    }
#else
    int fds[2];
    if (SDLOP_PipeCreate(fds, true, true) == 0) {
        int fl = fcntl(fds[1], F_GETFL);
        if (fl >= 0) {
            fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);
        }
        *rfd = fds[0];
        if (wfd) {
            *wfd = fds[1];
        }
    } else {
        *rfd = -1;
        if (wfd) {
            *wfd = -1;
        }
    }
#endif
}

static inline void SDLOP_WakeFdSignal(int rfd, int wfd)
{
#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
    (void)wfd;
    if (rfd >= 0) {
        uint64_t one = 1;
        (void)!write(rfd, &one, sizeof(one));
    }
#else
    (void)rfd;
    if (wfd >= 0) {
        char b = 1;
        (void)!write(wfd, &b, 1);
    }
#endif
}

static inline void SDLOP_WakeFdReset(int rfd)
{
    if (rfd < 0) {
        return;
    }
#if defined(SDLOP_HAVE_EVENTFD) && SDLOP_HAVE_EVENTFD
    uint64_t val;
    (void)!read(rfd, &val, sizeof(val));
#else
    char buf[64];
    while (read(rfd, buf, sizeof(buf)) > 0) {
    }
#endif
}

static inline void SDLOP_WakeFdClose(int *rfd, int *wfd)
{
    if (*rfd >= 0) {
        close(*rfd);
        *rfd = -1;
    }
    if (wfd && *wfd >= 0) {
        close(*wfd);
        *wfd = -1;
    }
}

/* Size a temp/SHM fd safely using posix_fallocate (blocking SIGALRM,
 * matching SDL3's SDL_waylandshmbuffer.c) with ftruncate fallback. */
static inline bool SDLOP_SetTempFileSize(int fd, off_t size)
{
#if defined(SDLOP_HAVE_POSIX_FALLOCATE) && SDLOP_HAVE_POSIX_FALLOCATE
    sigset_t set, old_set;
    int ret;

    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &old_set);
    do {
        ret = posix_fallocate(fd, 0, size);
    } while (ret == EINTR);
    sigprocmask(SIG_SETMASK, &old_set, NULL);

    if (ret == 0) {
        return true;
    }
    if (ret != EINVAL && ret != EOPNOTSUPP && errno != EINVAL && errno != EOPNOTSUPP) {
        return false;
    }
#endif
    return ftruncate(fd, size) >= 0;
}

/* Create an anonymous shared-memory fd sized to `size` bytes (for wl_shm).
 * Zero overhead on Linux / FreeBSD 13+ (direct memfd_create + sealing +
 * sizing); falls back to shm_open()+shm_unlink() (or mkostemp/mkstemp in
 * $XDG_RUNTIME_DIR like SDL3) elsewhere. */
static inline int SDLOP_ShmCreate(const char *name, size_t size)
{
    int fd = -1;
#if defined(SDLOP_HAVE_MEMFD_CREATE) && SDLOP_HAVE_MEMFD_CREATE
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0
#endif
#ifdef MFD_ALLOW_SEALING
    fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
#if defined(F_ADD_SEALS) && defined(F_SEAL_SHRINK) && defined(F_SEAL_SEAL)
    if (fd >= 0) {
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    }
#endif
#else
    fd = memfd_create(name, MFD_CLOEXEC);
#endif
#endif
#if !defined(SDLOP_HAVE_MEMFD_CREATE) || !SDLOP_HAVE_MEMFD_CREATE
    (void)name;
#endif
#if defined(SDLOP_HAVE_SHM_OPEN) && SDLOP_HAVE_SHM_OPEN
    if (fd < 0) {
        static unsigned int shm_seq;
        for (int attempt = 0; attempt < 32; attempt++) {
            char shm_name[64];
            unsigned int seq = __atomic_fetch_add(&shm_seq, 1u, __ATOMIC_RELAXED);
            snprintf(shm_name, sizeof(shm_name), "/sdlop-%ld-%u", (long)getpid(), seq);
            fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR | SDLOP_O_CLOEXEC, 0600);
            if (fd >= 0) {
                shm_unlink(shm_name);
#if !defined(O_CLOEXEC) && defined(FD_CLOEXEC)
                {
                    int fl = fcntl(fd, F_GETFD);
                    if (fl >= 0) {
                        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
                    }
                }
#endif
                break;
            }
            if (errno != EEXIST) {
                break;
            }
        }
    }
#endif
    if (fd < 0) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) {
            char tmp_path[1024];
            int n = snprintf(tmp_path, sizeof(tmp_path), "%s/sdlop-shm-XXXXXX", xdg);
            if (n > 0 && (size_t)n < sizeof(tmp_path)) {
#if defined(SDLOP_HAVE_MKOSTEMP) && SDLOP_HAVE_MKOSTEMP
                fd = mkostemp(tmp_path, SDLOP_O_CLOEXEC);
#else
                fd = mkstemp(tmp_path);
                if (fd >= 0) {
                    int fl = fcntl(fd, F_GETFD);
                    if (fl >= 0) {
                        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
                    }
                }
#endif
                if (fd >= 0) {
                    unlink(tmp_path);
                }
            }
        }
    }
    if (fd < 0) {
        return -1;
    }
    if (!SDLOP_SetTempFileSize(fd, (off_t)size)) {
        close(fd);
        return -1;
    }
    return fd;
}

#endif /* !_WIN32 && !__EMSCRIPTEN__ */

#endif /* sdlop_portable_h_ */
