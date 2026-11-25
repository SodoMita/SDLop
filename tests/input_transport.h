/* Shared bounded writer for native test/benchmark input records.
 * zlib license, see LICENSE. Not part of the production input worker. */
#ifndef sdlop_test_input_transport_h_
#define sdlop_test_input_transport_h_

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static bool input_write_record(int fd, const void *record, size_t bytes)
{
    uint64_t deadline = 0;
    for (;;) {
        ssize_t n = write(fd, record, bytes);
        if (n == (ssize_t)bytes) {
            return true;
        }
        if (n >= 0) {
            errno = EIO;
            return false; /* a native record must not be partially accepted */
        }
        int error = errno;
        if (error != EINTR && error != EAGAIN && error != EWOULDBLOCK) {
            return false;
        }
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
            return false;
        }
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (!deadline) {
            deadline = now + 1000000000ull;
        }
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        if (error == EINTR) {
            continue;
        }
        struct pollfd output = { .fd = fd, .events = POLLOUT };
        int timeout_ms = (int)((deadline - now + 999999ull) / 1000000ull);
        int ready = poll(&output, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) {
            continue; /* retain the absolute deadline, including signal time */
        }
        if (ready > 0 && (output.revents & POLLOUT) &&
            !(output.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            continue;
        }
        if (!ready) { errno = ETIMEDOUT; }
        else if (ready > 0) { errno = EIO; }
        return false;
    }
}
#endif
