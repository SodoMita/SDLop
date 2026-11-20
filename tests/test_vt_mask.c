/* No VT/device needed: only the driver's per-signal mask ownership helper. */
#include "internal/sdlop_vt.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    sigset_t original, before, owned, after;
    assert(pthread_sigmask(SIG_BLOCK, NULL, &original) == 0);
    sigemptyset(&before);
    sigaddset(&before, SIGUSR1); /* application already blocked a VT signal */
    sigaddset(&before, SIGHUP);  /* unrelated block before video init */
    assert(pthread_sigmask(SIG_SETMASK, &before, NULL) == 0);
    sigemptyset(&owned);
    sigaddset(&owned, SIGUSR1);
    sigaddset(&owned, SIGUSR2);
    assert(pthread_sigmask(SIG_BLOCK, &owned, NULL) == 0);

    sigset_t edit;
    sigemptyset(&edit);
    sigaddset(&edit, SIGHUP);
    assert(pthread_sigmask(SIG_UNBLOCK, &edit, NULL) == 0);
    sigemptyset(&edit);
    sigaddset(&edit, SIGTERM);
    assert(pthread_sigmask(SIG_BLOCK, &edit, NULL) == 0);

    assert(SDLOP_VTRestoreSignalMask(&before, SIGUSR1, SIGUSR2) == 0);
    assert(pthread_sigmask(SIG_BLOCK, NULL, &after) == 0);
    assert(sigismember(&after, SIGUSR1) == 1);
    assert(sigismember(&after, SIGUSR2) == 0);
    assert(sigismember(&after, SIGHUP) == 0);
    assert(sigismember(&after, SIGTERM) == 1);
    assert(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    puts("VT mask: owned blocks removed, existing block and application edits preserved");
    return 0;
}
