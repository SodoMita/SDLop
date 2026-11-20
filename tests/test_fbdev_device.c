/* Synthetic framebuffer geometry/layout regression; real mmap, fake FBIOGET*.
   Linked with --wrap=ioctl, never accesses a physical framebuffer or VT. */
#include <SDL3/SDL.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static struct fb_fix_screeninfo fix;
static struct fb_var_screeninfo var;
static struct stat backing;
static int failures;

int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_dev == backing.st_dev && st.st_ino == backing.st_ino) {
        if (request == FBIOGET_FSCREENINFO) {
            memcpy(arg, &fix, sizeof(fix));
            return 0;
        }
        if (request == FBIOGET_VSCREENINFO) {
            memcpy(arg, &var, sizeof(var));
            return 0;
        }
    }
    return __real_ioctl(fd, request, arg);
}

static void check(int ok, const char *message)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

static void layout(int bpp)
{
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    fix.type = FB_TYPE_PACKED_PIXELS;
    fix.visual = FB_VISUAL_TRUECOLOR;
    fix.line_length = 80 * (bpp / 8);
    fix.smem_len = fix.line_length * 64;
    var.bits_per_pixel = bpp;
    var.xres = 64;
    var.yres = 32;
    var.xres_virtual = 80;
    var.yres_virtual = 64;
    var.xoffset = 8;
    var.yoffset = 16;
    var.red = (struct fb_bitfield){ bpp == 16 ? 11 : 16, bpp == 16 ? 5 : 8, 0 };
    var.green = (struct fb_bitfield){ bpp == 16 ? 5 : 8, bpp == 16 ? 6 : 8, 0 };
    var.blue = (struct fb_bitfield){ 0, bpp == 16 ? 5 : 8, 0 };
}

static void rejected(const char *reason)
{
    const int initialized = SDL_Init(SDL_INIT_VIDEO);
    check(!initialized && strstr(SDL_GetError(), reason) != NULL, reason);
    if (initialized) {
        fprintf(stderr, "unexpectedly accepted invalid layout\n");
    } else {
        printf("  %s\n", SDL_GetError());
    }
    SDL_Quit();
}

int main(void)
{
    char path[] = "/tmp/sdlop-fbdev-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const size_t size = 80 * 4 * 64;
    assert(ftruncate(fd, size) == 0 && fstat(fd, &backing) == 0);
    unsigned char *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(pixels != MAP_FAILED);
    setenv("SDL_VIDEODRIVER", "fbdev", 1);
    setenv("SDLOP_FBDEV_PATH", path, 1);
    setenv("SDLOP_FBDEV_TAKEOVER", "0", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    unsetenv("SDLOP_FBDEV_DUMP");

    layout(32);
    var.red = (struct fb_bitfield){20, 10, 0};
    var.green = (struct fb_bitfield){10, 10, 0};
    var.blue = (struct fb_bitfield){0, 10, 0};
    rejected("channel wider than 8 bits");
    layout(32); fix.visual = FB_VISUAL_PSEUDOCOLOR;
    rejected("requires a packed true-colour visual");
    layout(32); var.grayscale = 1;
    rejected("nonstandard or greyscale mode");
    layout(32); var.red.length = 0;
    rejected("empty RGB channel");
    layout(32); var.green.msb_right = 1;
    rejected("reversed channel bit order");
    layout(32); var.red.offset = 30;
    rejected("channel extends beyond the pixel");
    layout(32); var.green.offset = var.red.offset;
    rejected("overlapping colour channels");
    layout(32); var.yoffset = 64;
    rejected("panning offset is out of range");

    for (int bpp = 16; bpp <= 32; bpp += 8) {
        layout(bpp);
        memset(pixels, 0x5a, size);
        const int initialized = SDL_Init(SDL_INIT_VIDEO);
        check(initialized, "valid packed true-colour mode initializes");
        if (!initialized) { continue; }
        SDL_Window *w = SDL_CreateWindow("synthetic framebuffer", 64, 32, 0);
        SDL_Surface *surface = w ? SDL_GetWindowSurface(w) : NULL;
        check(surface != NULL, "RAM surface created");
        if (surface) {
            SDL_HideCursor();
            SDL_FillSurfaceRect(surface, NULL, 0x0000c000);
            check(SDL_UpdateWindowSurface(w), "present into panned framebuffer");
            unsigned packed = 0;
            const size_t offset = var.yoffset * fix.line_length + var.xoffset * (bpp / 8);
            memcpy(&packed, pixels + offset, bpp / 8);
            check(packed == (bpp == 16 ? 0x600u : 0xc000u), "green packed at initial x/y offset");
            int untouched = 1;
            for (size_t i = 0; i < offset; i++) {
                if (pixels[i] != 0x5a) { untouched = 0; break; }
            }
            check(untouched, "rows and pixels before visible pan remain untouched");
        }
        SDL_DestroyWindow(w);
        SDL_Quit();
        printf("  exercised %d bpp\n", bpp);
    }
    munmap(pixels, size);
    close(fd);
    unlink(path);
    return failures ? 1 : 0;
}
