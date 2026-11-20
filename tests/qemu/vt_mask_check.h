/* Regression fixture: application mask edits after SDL_Init survive SDL_Quit. */
#ifndef TEST_VT_MASK_CHECK_H
#define TEST_VT_MASK_CHECK_H

#include <pthread.h>
#include <signal.h>

struct vt_mask_probe {
    sigset_t original, before_init;
};

static int vt_mask_begin(struct vt_mask_probe *p)
{
    sigset_t one;
    if (pthread_sigmask(SIG_BLOCK, NULL, &p->original) != 0) {
        return 0;
    }
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &one, NULL) != 0) {
        return 0;
    }
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    return pthread_sigmask(SIG_UNBLOCK, &one, NULL) == 0 &&
           pthread_sigmask(SIG_BLOCK, NULL, &p->before_init) == 0;
}

static int vt_mask_edit(void)
{
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &one, NULL) != 0) {
        return 0;
    }
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    return pthread_sigmask(SIG_BLOCK, &one, NULL) == 0;
}

static int vt_mask_finish(struct vt_mask_probe *p, int rel, int acq)
{
    sigset_t after;
    int ok = pthread_sigmask(SIG_BLOCK, NULL, &after) == 0;
    if (ok) {
        ok = sigismember(&after, SIGUSR1) == 0 && sigismember(&after, SIGUSR2) == 1;
        if (rel > 0) {
            ok &= sigismember(&after, rel) == sigismember(&p->before_init, rel);
        }
        if (acq > 0) {
            ok &= sigismember(&after, acq) == sigismember(&p->before_init, acq);
        }
    }
    /* Only the fixture restores its complete original mask, not the driver. */
    return pthread_sigmask(SIG_SETMASK, &p->original, NULL) == 0 && ok;
}
#endif
