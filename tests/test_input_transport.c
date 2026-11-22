/* Native benchmark writer failures must not look like delivered input.
 * zlib license, see LICENSE. No devices or production library required. */
#include "input_transport.h"
#include <assert.h>
#include <stdio.h>

enum Mode { GOOD, INTERRUPT, BACKPRESSURE, TIMEOUT, POLL_INTERRUPT,
            POLL_ERROR, SHORT, HARD_ERROR, SPURIOUS_READY };
static enum Mode mode;
static int writes, polls, clocks;

ssize_t __wrap_write(int fd, const void *record, size_t bytes)
{
    (void)fd; (void)record;
    writes++;
    if (mode == HARD_ERROR) { errno = EPIPE; return -1; }
    if (mode == SHORT) { return (ssize_t)bytes - 1; }
    if (mode == SPURIOUS_READY || (writes == 1 && mode != GOOD)) {
        errno = mode == INTERRUPT ? EINTR : EAGAIN;
        return -1;
    }
    return bytes;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && timeout > 0 && timeout <= 1000);
    polls++;
    if (mode == TIMEOUT) { return 0; }
    if (mode == POLL_INTERRUPT) { errno = EINTR; return -1; }
    fds[0].revents = mode == POLL_ERROR ? POLLERR : POLLOUT;
    return 1;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    assert(clock == CLOCK_MONOTONIC);
    /* Deterministically exhaust a one-second budget, without sleeping. */
    clocks++;
    ts->tv_sec = 10 + clocks / 2;
    ts->tv_nsec = (clocks % 2) * 500000000;
    return 0;
}

int main(void)
{
    char record[24] = { 0 };
    for (mode = GOOD; mode <= SPURIOUS_READY; mode++) {
        writes = polls = clocks = 0;
        bool ok = input_write_record(123, record, sizeof(record));
        bool expected = mode == GOOD || mode == INTERRUPT || mode == BACKPRESSURE || mode == POLL_INTERRUPT;
        assert(ok == expected);
        if (mode == GOOD) { assert(writes == 1 && !polls && !clocks); }
        if (mode == INTERRUPT) { assert(writes == 2 && !polls); }
        if (mode == BACKPRESSURE || mode == POLL_INTERRUPT) { assert(writes == 2 && polls == 1); }
        if (mode == TIMEOUT || mode == SPURIOUS_READY) { assert(errno == ETIMEDOUT); }
        if (mode == SPURIOUS_READY) { assert(writes == 3 && polls == 2); }
        if (mode == SHORT || mode == POLL_ERROR) { assert(errno == EIO); }
        if (mode == HARD_ERROR) { assert(errno == EPIPE); }
    }
    puts("test_input_transport: PASS (writes, EINTR, backpressure, timeout, short/error, spurious readiness)");
    return 0;
}
