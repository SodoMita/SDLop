/*
  Simulates a console that already belongs to another program, then starts SDL
  on top of it. The driver must refuse to adopt a KD_GRAPHICS / VT_PROCESS VT:
  it keeps presents in RAM unless --allow explicitly requests takeover=0.
  It must not change that program's console state: no KD_TEXT, K_*KBMODE or
  VT_AUTO on the way out.

  Run by tests/run_fbdev_qemu.sh (SDLOP_FBDEV_OWNED_CHECK=1); needs root and a
  console that nothing else has claimed at that moment.
*/
#include <SDL3/SDL.h>

#include <signal.h>
#include <stdarg.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(const char *name, int ok, const char *fmt, ...)
{
    va_list ap;
    printf("VT-CHECK %s %s ", name, ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) {
        failures++;
    }
}

static int owner_vt(void)
{
    /* VT 2 is the one the harness leaves the user on */
    return open("/dev/tty2", O_RDWR | O_NOCTTY | O_CLOEXEC);
}

int main(int argc, char **argv)
{
    const bool allow = argc == 2 && strcmp(argv[1], "--allow") == 0;
    setenv("SDLOP_FBDEV_TAKEOVER", allow ? "0" : "1", 1);
    const int fd = owner_vt();
    if (fd < 0) {
        printf("VT-CHECK owned-skipped SKIP cannot open /dev/tty2: %s\n", strerror(errno));
        return 0;
    }

    /* another program takes the console exactly the way SDLop does */
    struct vt_mode mode;
    memset(&mode, 0, sizeof(mode));
    mode.mode = VT_PROCESS;
    mode.relsig = SIGUSR1;
    mode.acqsig = SIGUSR2;
    if (ioctl(fd, KDSETMODE, KD_GRAPHICS) != 0 || ioctl(fd, VT_SETMODE, &mode) != 0) {
        printf("VT-CHECK owned-skipped SKIP cannot take the console: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    const int kbmode_before = K_XLATE;
    if (ioctl(fd, KDSKBMODE, kbmode_before) != 0) {
        printf("VT-CHECK owned-skipped SKIP cannot set the console keyboard: %s\n", strerror(errno));
        ioctl(fd, KDSETMODE, KD_TEXT);
        close(fd);
        return 0;
    }
    int mode_before = -1;
    if (ioctl(fd, KDGETMODE, &mode_before) != 0 || mode_before != KD_GRAPHICS) {
        printf("VT-CHECK owned-skipped SKIP KDGETMODE=%d (%s)\n", mode_before, strerror(errno));
        ioctl(fd, KDSETMODE, KD_TEXT);
        close(fd);
        return 0;
    }

    /* Snapshot the whole framebuffer while the fake owner holds KD_GRAPHICS:
       refusal must suppress show, present, cursor and quit writes, not just
       avoid KD/VT ioctls. An explicit takeover=0 request is the sole override. */
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) != 0 ||
        ioctl(fb, FBIOGET_VSCREENINFO, &var) != 0) {
        check("owned-scanout-probe", 0, "cannot inspect /dev/fb0");
        return 1;
    }
    const size_t length = (size_t)fix.line_length * var.yres_virtual;
    void *pixels = mmap(NULL, length, PROT_READ, MAP_SHARED, fb, 0);
    void *snapshot = malloc(length);
    if (pixels == MAP_FAILED || !snapshot) {
        check("owned-scanout-map", 0, "cannot snapshot /dev/fb0");
        return 1;
    }
    memcpy(snapshot, pixels, length);

    setenv("SDL_VIDEODRIVER", "fbdev", 1);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "fbdev");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        check("owned-init", 0, "SDL_Init failed: %s", SDL_GetError());
        close(fd);
        return 1;
    }
    check("owned-init", 1, "SDL started on a console it does not own");

    SDL_Window *w = SDL_CreateWindow("owned", 320, 240, 0);
    check("owned-window", w != NULL, "%s", w ? "created" : SDL_GetError());
    if (w) {
        SDL_Surface *s = SDL_GetWindowSurface(w);
        check("owned-surface", s != NULL, "%s", s ? "usable" : SDL_GetError());
        if (s) {
            SDL_FillSurfaceRect(s, NULL, 0x00804020u);
            check("owned-present", SDL_UpdateWindowSurface(w),
                  allow ? "explicit direct present accepted" : "present retained in RAM");
        }
    }

    SDL_ShowCursor();
    SDL_WarpMouseInWindow(w, 64, 64);
    SDL_PumpEvents();
    check("owned-scanout-during", (memcmp(snapshot, pixels, length) != 0) == allow,
          allow ? "explicit takeover=0 allowed scanout writes" : "show/present/cursor left owner's pixels untouched");

    /* the other program's console state must be untouched */
    const int probe = owner_vt();
    if (probe < 0) {
        check("owned-probe", 0, "cannot re-open /dev/tty2: %s", strerror(errno));
        close(fd);
        return 1;
    }
    int m = -1, kb = -1;
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    const int mrc = ioctl(probe, KDGETMODE, &m);
    check("owned-mode-kept", mrc == 0 && m == mode_before,
          "KD_GRAPHICS is still there (got %d, was %d, rc=%d errno=%s)", m, mode_before, mrc,
          strerror(errno));
    const int kbrc = ioctl(probe, KDGKBMODE, &kb);
    check("owned-kbd-kept", kbrc == 0 && kb == kbmode_before,
          "the console keyboard mode was left alone (got %d, want %d)", kb, kbmode_before);
    const int vmrc = ioctl(probe, VT_GETMODE, &vm);
    check("owned-vt-mode-kept", vmrc == 0 && vm.mode == VT_PROCESS,
          "VT_PROCESS still belongs to its owner (got %d)", (int)vm.mode);
    check("owned-signals-kept", vm.relsig == SIGUSR1 && vm.acqsig == SIGUSR2,
          "release/acquire signals unchanged (relsig=%d acqsig=%d)", (int)vm.relsig, (int)vm.acqsig);

    if (w) {
        SDL_DestroyWindow(w);
    }
    SDL_Quit();

    /* and after our quit, too */
    m = kb = -1;
    memset(&vm, 0, sizeof(vm));
    const int m2rc = ioctl(probe, KDGETMODE, &m);
    check("owned-mode-after", m2rc == 0 && m == mode_before,
          "KD_TEXT was not forced on the owner (got %d)", m);
    const int kb2rc = ioctl(probe, KDGKBMODE, &kb);
    check("owned-kbd-after", kb2rc == 0 && kb == kbmode_before,
          "the owner's keyboard mode survived our quit (got %d)", kb);
    const int vm2rc = ioctl(probe, VT_GETMODE, &vm);
    check("owned-vt-mode-after", vm2rc == 0 && vm.mode == VT_PROCESS,
          "VT_AUTO was not forced on the owner (got %d)", (int)vm.mode);

    check("owned-signals-after", vm.relsig == SIGUSR1 && vm.acqsig == SIGUSR2,
          "owner's release/acquire signals survived quit");
    check("owned-scanout-after", (memcmp(snapshot, pixels, length) != 0) == allow,
          allow ? "direct-render override exercised" : "quit left owner's scanout untouched");
    munmap(pixels, length);
    free(snapshot);
    close(fb);
    close(probe);
    /* hand the console back the way we found it */
    ioctl(fd, KDSETMODE, KD_TEXT);
    memset(&mode, 0, sizeof(mode));
    mode.mode = VT_AUTO;
    ioctl(fd, VT_SETMODE, &mode);
    close(fd);
    printf("VT-CHECK owned-done %s %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
