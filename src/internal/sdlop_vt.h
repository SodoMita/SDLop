/* Console signal-mask ownership. Keep unrelated application mask edits intact. */
#ifndef SDLOP_VT_H
#define SDLOP_VT_H

#include <pthread.h>
#include <signal.h>

/* Undo only the blocks we added. Signals already blocked before init stay
   under application control; we never restore a whole, stale thread mask. */
static inline int SDLOP_VTRestoreSignalMask(const sigset_t *before, int release_sig, int acquire_sig)
{
    sigset_t unblock;
    sigemptyset(&unblock);
    if (release_sig > 0 && sigismember(before, release_sig) == 0) {
        sigaddset(&unblock, release_sig);
    }
    if (acquire_sig > 0 && sigismember(before, acquire_sig) == 0) {
        sigaddset(&unblock, acquire_sig);
    }
    return pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
}

#endif
