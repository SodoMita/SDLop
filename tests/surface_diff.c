/*
  SDLop: surface/pixels/rect differential probe.

  Runs a deterministic battery of SDL_pixels.h / SDL_rect.h / SDL_surface.h
  operations and prints one hash line per case. Build it once against
  SDLop and once against stock SDL3 and diff the output
  (tests/run_surface_diff.sh); with no reference library it still runs as
  a crash/sanitizer smoke test over every supported format and blend mode.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint32 rng_state = 0x12345678u;
static Uint32 rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static Uint64 fnv(Uint64 h, const void *data, size_t len)
{
    const Uint8 *p = (const Uint8 *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static SDL_Surface *last_hashed;

/* hash only the meaningful bits: X channels and row padding are undefined */
static Uint64 hash_surface(SDL_Surface *s)
{
    last_hashed = s;
    Uint64 h = 0xcbf29ce484222325ull;
    const SDL_PixelFormatDetails *d = SDL_GetPixelFormatDetails(s->format);
    const int bpp = SDL_BYTESPERPIXEL(s->format);
    const bool indexed = SDL_ISPIXELFORMAT_INDEXED(s->format);
    Uint32 mask = indexed ? 0xFFFFFFFFu : (d->Rmask | d->Gmask | d->Bmask | d->Amask);
    if (SDL_ISPIXELFORMAT_10BIT(s->format)) {
        mask = SDL_ISPIXELFORMAT_ALPHA(s->format) ? 0xFFFFFFFFu : 0x3FFFFFFFu;
    }
    for (int y = 0; y < s->h; ++y) {
        const Uint8 *row = (const Uint8 *)s->pixels + (size_t)y * s->pitch;
        if (bpp > 4) { /* 48/64/96/128-bit formats have no padding channels */
            h = fnv(h, row, (size_t)s->w * bpp);
            continue;
        }
        if (SDL_BITSPERPIXEL(s->format) < 8) {
            h = fnv(h, row, (size_t)(s->w * SDL_BITSPERPIXEL(s->format) + 7) / 8);
            continue;
        }
        for (int x = 0; x < s->w; ++x) {
            Uint32 p = 0;
            memcpy(&p, row + x * bpp, bpp);
            p &= mask;
            h = fnv(h, &p, sizeof(p));
        }
    }
    return h;
}

static const SDL_PixelFormat formats[] = {
    SDL_PIXELFORMAT_INDEX8,
    SDL_PIXELFORMAT_RGB332,
    SDL_PIXELFORMAT_XRGB4444, SDL_PIXELFORMAT_ARGB4444, SDL_PIXELFORMAT_RGBA4444,
    SDL_PIXELFORMAT_XRGB1555, SDL_PIXELFORMAT_ARGB1555, SDL_PIXELFORMAT_RGBA5551,
    SDL_PIXELFORMAT_RGB565, SDL_PIXELFORMAT_BGR565,
    SDL_PIXELFORMAT_RGB24, SDL_PIXELFORMAT_BGR24,
    SDL_PIXELFORMAT_XRGB8888, SDL_PIXELFORMAT_RGBX8888, SDL_PIXELFORMAT_XBGR8888, SDL_PIXELFORMAT_BGRX8888,
    SDL_PIXELFORMAT_ARGB8888, SDL_PIXELFORMAT_RGBA8888, SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_BGRA8888,
    SDL_PIXELFORMAT_XRGB2101010, SDL_PIXELFORMAT_ARGB2101010, SDL_PIXELFORMAT_ABGR2101010,
};
#define NFORMATS ((int)(sizeof(formats) / sizeof(formats[0])))

static const SDL_BlendMode blends[] = {
    SDL_BLENDMODE_NONE, SDL_BLENDMODE_BLEND, SDL_BLENDMODE_BLEND_PREMULTIPLIED,
    SDL_BLENDMODE_ADD, SDL_BLENDMODE_ADD_PREMULTIPLIED, SDL_BLENDMODE_MOD, SDL_BLENDMODE_MUL
};

static SDL_Palette *make_palette(int n)
{
    SDL_Palette *pal = SDL_CreatePalette(n);
    SDL_Color c[256];
    for (int i = 0; i < n; ++i) {
        c[i].r = (Uint8)rnd();
        c[i].g = (Uint8)rnd();
        c[i].b = (Uint8)rnd();
        c[i].a = (Uint8)rnd();
    }
    SDL_SetPaletteColors(pal, c, 0, n);
    return pal;
}

static SDL_Surface *make_surface(SDL_PixelFormat fmt, int w, int h, SDL_Palette *pal)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, fmt);
    if (!s) {
        printf("create %s failed: %s\n", SDL_GetPixelFormatName(fmt), SDL_GetError());
        return NULL;
    }
    if (SDL_ISPIXELFORMAT_INDEXED(fmt)) {
        SDL_SetSurfacePalette(s, pal);
    }
    for (int y = 0; y < h; ++y) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < s->pitch; ++x) {
            row[x] = (Uint8)rnd();
        }
    }
    /* a few exact extremes so blending hits 0 and 255 alpha */
    SDL_WriteSurfacePixel(s, 0, 0, 255, 255, 255, 255);
    SDL_WriteSurfacePixel(s, 1, 0, 0, 0, 0, 0);
    SDL_WriteSurfacePixel(s, 2, 0, 200, 100, 50, 128);
    return s;
}

static int case_no = 0;
static void out(const char *what, Uint64 h)
{
    /* SURFACE_DIFF_DUMP=<case> prints the hashed surface of that case,
       for tracking down a mismatch pixel by pixel */
    static int dump_case = -2;
    if (dump_case == -2) {
        const char *env = getenv("SURFACE_DIFF_DUMP");
        dump_case = env ? atoi(env) : -1;
    }
    if (case_no == dump_case && last_hashed) {
        SDL_Surface *d = last_hashed;
        const int bpp = SDL_BYTESPERPIXEL(d->format);
        for (int y = 0; y < d->h; ++y) {
            for (int x = 0; x < d->w; ++x) {
                Uint32 p = 0;
                memcpy(&p, (const Uint8 *)d->pixels + (size_t)y * d->pitch + x * bpp, bpp);
                printf(" %0*x", bpp * 2, p);
            }
            printf("\n");
        }
    }
    last_hashed = NULL;
    printf("%04d %s %016llx\n", case_no++, what, (unsigned long long)h);
}

static void test_pixels(void)
{
    for (int i = 0; i < NFORMATS; ++i) {
        const SDL_PixelFormatDetails *d = SDL_GetPixelFormatDetails(formats[i]);
        int bpp;
        Uint32 r, g, b, a;
        SDL_GetMasksForPixelFormat(formats[i], &bpp, &r, &g, &b, &a);
        printf("fmt %-24s bpp=%d masks=%08x %08x %08x %08x back=%s Bpp=%d\n", SDL_GetPixelFormatName(formats[i]),
               bpp, r, g, b, a, SDL_GetPixelFormatName(SDL_GetPixelFormatForMasks(bpp, r, g, b, a)), d ? d->bytes_per_pixel : -1);
        if (!SDL_ISPIXELFORMAT_INDEXED(formats[i])) {
            Uint64 h = 0xcbf29ce484222325ull;
            for (int k = 0; k < 512; ++k) {
                Uint8 c[4] = { (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd() };
                Uint32 p = SDL_MapRGBA(d, NULL, c[0], c[1], c[2], c[3]);
                Uint8 o[4];
                SDL_GetRGBA(p, d, NULL, &o[0], &o[1], &o[2], &o[3]);
                h = fnv(h, &p, 4);
                h = fnv(h, o, 4);
                p = SDL_MapRGB(d, NULL, c[0], c[1], c[2]);
                SDL_GetRGB(p, d, NULL, &o[0], &o[1], &o[2]);
                h = fnv(h, &p, 4);
                h = fnv(h, o, 3);
            }
            out("maprgba", h);
        }
    }
    printf("names %s %s %s\n", SDL_GetPixelFormatName(SDL_PIXELFORMAT_RGBA32), SDL_GetPixelFormatName((SDL_PixelFormat)0x12345678),
           SDL_GetPixelFormatName(SDL_PIXELFORMAT_NV12));
    SDL_Palette *pal = make_palette(256);
    const SDL_PixelFormatDetails *d8 = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_INDEX8);
    Uint64 h = 0xcbf29ce484222325ull;
    for (int k = 0; k < 256; ++k) {
        Uint8 c[4] = { (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd() };
        Uint32 p = SDL_MapRGBA(d8, pal, c[0], c[1], c[2], c[3]);
        Uint8 o[4];
        SDL_GetRGBA(p, d8, pal, &o[0], &o[1], &o[2], &o[3]);
        h = fnv(h, &p, 4);
        h = fnv(h, o, 4);
    }
    out("palette-map", h);
    SDL_DestroyPalette(pal);
}

static void test_rects(void)
{
    Uint64 h = 0xcbf29ce484222325ull;
    for (int k = 0; k < 2000; ++k) {
        SDL_Rect a = { (int)(rnd() % 40) - 10, (int)(rnd() % 40) - 10, (int)(rnd() % 30) - 2, (int)(rnd() % 30) - 2 };
        SDL_Rect b = { (int)(rnd() % 40) - 10, (int)(rnd() % 40) - 10, (int)(rnd() % 30) - 2, (int)(rnd() % 30) - 2 };
        SDL_Rect r = { 0, 0, 0, 0 };
        int ret[4];
        ret[0] = SDL_HasRectIntersection(&a, &b);
        ret[1] = SDL_GetRectIntersection(&a, &b, &r);
        h = fnv(h, &r, sizeof(r));
        memset(&r, 0, sizeof(r));
        ret[2] = SDL_GetRectUnion(&a, &b, &r);
        h = fnv(h, &r, sizeof(r));
        int x1 = (int)(rnd() % 60) - 15, y1 = (int)(rnd() % 60) - 15, x2 = (int)(rnd() % 60) - 15, y2 = (int)(rnd() % 60) - 15;
        ret[3] = SDL_GetRectAndLineIntersection(&a, &x1, &y1, &x2, &y2);
        int line[4] = { x1, y1, x2, y2 };
        h = fnv(h, line, sizeof(line));
        h = fnv(h, ret, sizeof(ret));

        SDL_FRect fa = { (float)a.x * 0.5f, (float)a.y * 0.75f, (float)a.w * 1.25f, (float)a.h };
        SDL_FRect fb = { (float)b.x, (float)b.y * 0.5f, (float)b.w, (float)b.h * 1.5f };
        SDL_FRect fr = { 0, 0, 0, 0 };
        ret[0] = SDL_HasRectIntersectionFloat(&fa, &fb);
        ret[1] = SDL_GetRectIntersectionFloat(&fa, &fb, &fr);
        h = fnv(h, &fr, sizeof(fr));
        ret[2] = SDL_GetRectUnionFloat(&fa, &fb, &fr);
        h = fnv(h, &fr, sizeof(fr));
        float fx1 = (float)x1 * 0.5f, fy1 = (float)y1, fx2 = (float)x2, fy2 = (float)y2 * 0.5f;
        ret[3] = SDL_GetRectAndLineIntersectionFloat(&fa, &fx1, &fy1, &fx2, &fy2);
        float fl[4] = { fx1, fy1, fx2, fy2 };
        h = fnv(h, fl, sizeof(fl));
        h = fnv(h, ret, sizeof(ret));
    }
    out("rects", h);
    SDL_Point pts[16];
    SDL_FPoint fpts[16];
    for (int i = 0; i < 16; ++i) {
        pts[i].x = (int)(rnd() % 100) - 50;
        pts[i].y = (int)(rnd() % 100) - 50;
        fpts[i].x = (float)pts[i].x * 0.3f;
        fpts[i].y = (float)pts[i].y * 0.7f;
    }
    SDL_Rect clip = { -20, -20, 40, 40 }, r;
    SDL_FRect fclip = { -10, -10, 20, 20 }, fr;
    int rets[4];
    rets[0] = SDL_GetRectEnclosingPoints(pts, 16, NULL, &r);
    h = fnv(0, &r, sizeof(r));
    rets[1] = SDL_GetRectEnclosingPoints(pts, 16, &clip, &r);
    h = fnv(h, &r, sizeof(r));
    rets[2] = SDL_GetRectEnclosingPointsFloat(fpts, 16, NULL, &fr);
    h = fnv(h, &fr, sizeof(fr));
    rets[3] = SDL_GetRectEnclosingPointsFloat(fpts, 16, &fclip, &fr);
    h = fnv(h, &fr, sizeof(fr));
    h = fnv(h, rets, sizeof(rets));
    out("enclose", h);
}

static void test_blits(void)
{
    SDL_Palette *pal = make_palette(256);
    for (int si = 0; si < NFORMATS; ++si) {
        for (int di = 0; di < NFORMATS; ++di) {
            for (int bi = 0; bi < (int)SDL_arraysize(blends); ++bi) {
                char what[160];
                SDL_Surface *src = make_surface(formats[si], 13, 9, pal);
                SDL_Surface *dst = make_surface(formats[di], 17, 11, pal);
                if (!src || !dst) {
                    return;
                }
                SDL_SetSurfaceBlendMode(src, blends[bi]);
                int variant = (int)(rnd() % 4);
                if (variant & 1) {
                    SDL_SetSurfaceColorMod(src, (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd());
                    SDL_SetSurfaceAlphaMod(src, (Uint8)rnd());
                }
                if (variant & 2) {
                    Uint32 key = 0;
                    const Uint8 *p = (const Uint8 *)src->pixels + src->pitch;
                    memcpy(&key, p, SDL_BYTESPERPIXEL(src->format));
                    if (SDL_BYTESPERPIXEL(src->format) == 1) {
                        key &= 0xFF;
                    } else if (SDL_BYTESPERPIXEL(src->format) == 2) {
                        key &= 0xFFFF;
                    } else if (SDL_BYTESPERPIXEL(src->format) == 3) {
                        key &= 0xFFFFFF;
                    }
                    SDL_SetSurfaceColorKey(src, true, key);
                }
                SDL_Rect sr = { 1, 1, 11, 7 };
                SDL_Rect dr = { 3, 2, 0, 0 };
                bool ok = SDL_BlitSurface(src, &sr, dst, &dr);
                snprintf(what, sizeof(what), "blit %s->%s %d v%d ok=%d", SDL_GetPixelFormatName(formats[si]),
                         SDL_GetPixelFormatName(formats[di]), (int)blends[bi], variant, ok);
                out(what, hash_surface(dst));

                SDL_Rect sdr = { -2, 1, 21, 13 };
                ok = SDL_BlitSurfaceScaled(src, NULL, dst, &sdr, SDL_SCALEMODE_NEAREST);
                snprintf(what, sizeof(what), "scaled %s->%s %d v%d ok=%d", SDL_GetPixelFormatName(formats[si]),
                         SDL_GetPixelFormatName(formats[di]), (int)blends[bi], variant, ok);
                out(what, hash_surface(dst));
                SDL_DestroySurface(src);
                SDL_DestroySurface(dst);
            }
        }
    }
    SDL_DestroyPalette(pal);
}

static void test_convert(void)
{
    SDL_Palette *pal = make_palette(256);
    for (int si = 0; si < NFORMATS; ++si) {
        for (int di = 0; di < NFORMATS; ++di) {
            char what[160];
            SDL_Surface *src = make_surface(formats[si], 7, 5, pal);
            SDL_Surface *c = SDL_ConvertSurface(src, formats[di]);
            SDL_BlendMode bm = SDL_BLENDMODE_INVALID;
            if (c) {
                SDL_GetSurfaceBlendMode(c, &bm);
            }
            snprintf(what, sizeof(what), "convert %s->%s ok=%d bm=%d", SDL_GetPixelFormatName(formats[si]),
                     SDL_GetPixelFormatName(formats[di]), c != NULL, (int)bm);
            out(what, c ? hash_surface(c) : 0);
            SDL_DestroySurface(c);
            SDL_DestroySurface(src);
        }
    }
    SDL_DestroyPalette(pal);
}

static void test_scale_misc(void)
{
    static const SDL_PixelFormat f4[] = { SDL_PIXELFORMAT_ARGB8888, SDL_PIXELFORMAT_XRGB8888, SDL_PIXELFORMAT_RGB565, SDL_PIXELFORMAT_RGB24, SDL_PIXELFORMAT_INDEX8 };
    SDL_Palette *pal = make_palette(256);
    for (int i = 0; i < (int)SDL_arraysize(f4); ++i) {
        char what[128];
        SDL_Surface *src = make_surface(f4[i], 9, 7, pal);
        static const int sizes[][2] = { { 18, 14 }, { 4, 3 }, { 13, 5 }, { 1, 1 } };
        for (int k = 0; k < 4; ++k) {
            for (int m = 0; m < 2; ++m) {
                SDL_Surface *s = SDL_ScaleSurface(src, sizes[k][0], sizes[k][1], m ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                snprintf(what, sizeof(what), "scale %s %dx%d m%d ok=%d", SDL_GetPixelFormatName(f4[i]), sizes[k][0], sizes[k][1], m, s != NULL);
                out(what, s ? hash_surface(s) : 0);
                SDL_DestroySurface(s);
            }
        }
        for (int fl = 1; fl <= 3; ++fl) {
            SDL_Surface *d = SDL_DuplicateSurface(src);
            bool ok = SDL_FlipSurface(d, (SDL_FlipMode)fl);
            snprintf(what, sizeof(what), "flip %s %d ok=%d", SDL_GetPixelFormatName(f4[i]), fl, ok);
            out(what, hash_surface(d));
            SDL_DestroySurface(d);
        }
        SDL_Surface *d = SDL_DuplicateSurface(src);
        SDL_Rect rects[3] = { { -2, -2, 4, 4 }, { 3, 1, 3, 10 }, { 8, 6, 5, 5 } };
        bool ok = SDL_FillSurfaceRects(d, rects, 3, 0x12345678u & ((SDL_BYTESPERPIXEL(f4[i]) == 4) ? 0xFFFFFFFFu : (SDL_BYTESPERPIXEL(f4[i]) == 3 ? 0xFFFFFFu : SDL_BYTESPERPIXEL(f4[i]) == 2 ? 0xFFFFu : 0xFFu)));
        snprintf(what, sizeof(what), "fill %s ok=%d", SDL_GetPixelFormatName(f4[i]), ok);
        out(what, hash_surface(d));
        SDL_Rect clip = { 2, 2, 4, 3 };
        SDL_SetSurfaceClipRect(d, &clip);
        ok = SDL_ClearSurface(d, 0.25f, 0.5f, 0.75f, 1.0f);
        SDL_Rect got;
        SDL_GetSurfaceClipRect(d, &got);
        snprintf(what, sizeof(what), "clear %s ok=%d clip=%d,%d,%d,%d", SDL_GetPixelFormatName(f4[i]), ok, got.x, got.y, got.w, got.h);
        out(what, hash_surface(d));
        float fr, fg, fb, fa;
        Uint8 r, g, b, a;
        SDL_ReadSurfacePixel(d, 3, 3, &r, &g, &b, &a);
        SDL_ReadSurfacePixelFloat(d, 4, 4, &fr, &fg, &fb, &fa);
        printf("read %s %u %u %u %u %.4f %.4f %.4f %.4f\n", SDL_GetPixelFormatName(f4[i]), r, g, b, a, fr, fg, fb, fa);
        SDL_WriteSurfacePixelFloat(d, 0, 0, 0.1f, 0.2f, 0.3f, 0.4f);
        SDL_ReadSurfacePixel(d, 0, 0, &r, &g, &b, &a);
        printf("rw %s %u %u %u %u map=%08x\n", SDL_GetPixelFormatName(f4[i]), r, g, b, a, SDL_MapSurfaceRGBA(d, 10, 20, 30, 40));
        SDL_DestroySurface(d);

        if (SDL_BYTESPERPIXEL(f4[i]) == 4 || f4[i] == SDL_PIXELFORMAT_INDEX8) {
            if (f4[i] == SDL_PIXELFORMAT_INDEX8) {
                SDL_SetSurfaceColorKey(src, true, 3);
            }
            for (int ang = 0; ang < 4; ++ang) {
                static const float angles[] = { 90.0f, 33.0f, -180.0f, 271.5f };
                SDL_Surface *rot = SDL_RotateSurface(src, angles[ang]);
                snprintf(what, sizeof(what), "rotate %s %.1f ok=%d %dx%d", SDL_GetPixelFormatName(f4[i]), angles[ang], rot != NULL,
                         rot ? rot->w : 0, rot ? rot->h : 0);
                out(what, rot ? hash_surface(rot) : 0);
                SDL_DestroySurface(rot);
            }
        }
        SDL_DestroySurface(src);
    }

    /* tiled / 9-grid */
    SDL_Surface *src = make_surface(SDL_PIXELFORMAT_ARGB8888, 8, 8, pal);
    SDL_Surface *dst = make_surface(SDL_PIXELFORMAT_XRGB8888, 40, 30, pal);
    SDL_Rect sr = { 1, 1, 5, 6 }, dr = { 2, 3, 33, 21 };
    bool ok = SDL_BlitSurfaceTiled(src, &sr, dst, &dr);
    out(ok ? "tiled ok" : "tiled fail", hash_surface(dst));
    ok = SDL_BlitSurfaceTiledWithScale(src, &sr, 1.5f, SDL_SCALEMODE_NEAREST, dst, &dr);
    out(ok ? "tiledscale ok" : "tiledscale fail", hash_surface(dst));
    ok = SDL_BlitSurface9Grid(src, NULL, 2, 2, 3, 2, 1.0f, SDL_SCALEMODE_NEAREST, dst, &dr);
    out(ok ? "9grid ok" : "9grid fail", hash_surface(dst));
    ok = SDL_BlitSurface9Grid(src, NULL, 2, 2, 3, 2, 2.0f, SDL_SCALEMODE_LINEAR, dst, NULL);
    out(ok ? "9grid2 ok" : "9grid2 fail", hash_surface(dst));
    ok = SDL_BlitSurfaceScaled(src, &sr, dst, &dr, SDL_SCALEMODE_LINEAR);
    out(ok ? "linear ok" : "linear fail", hash_surface(dst));
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    ok = SDL_BlitSurfaceScaled(src, &sr, dst, &dr, SDL_SCALEMODE_LINEAR);
    out(ok ? "linear2 ok" : "linear2 fail", hash_surface(dst));
    ok = SDL_StretchSurface(src, NULL, dst, &dr, SDL_SCALEMODE_LINEAR);
    out(ok ? "stretch ok" : "stretch fail", hash_surface(dst));

    /* premultiply */
    SDL_Surface *pm = SDL_DuplicateSurface(src);
    ok = SDL_PremultiplySurfaceAlpha(pm, false);
    out(ok ? "premul ok" : "premul fail", hash_surface(pm));
    Uint8 buf[8 * 8 * 4];
    ok = SDL_PremultiplyAlpha(8, 8, SDL_PIXELFORMAT_ARGB8888, src->pixels, src->pitch, SDL_PIXELFORMAT_ABGR8888, buf, 32, false);
    out(ok ? "premul2 ok" : "premul2 fail", fnv(0, buf, sizeof(buf)));
    ok = SDL_ConvertPixels(8, 8, SDL_PIXELFORMAT_ARGB8888, src->pixels, src->pitch, SDL_PIXELFORMAT_RGB565, buf, 16);
    out(ok ? "convpix ok" : "convpix fail", fnv(0, buf, 16 * 8));
    SDL_DestroySurface(pm);

    /* self blit (overlap) */
    SDL_Rect a = { 0, 0, 30, 20 }, b = { 3, 2, 0, 0 };
    SDL_BlitSurface(dst, &a, dst, &b);
    out("selfblit", hash_surface(dst));

    /* properties, images, colorkey state */
    SDL_Surface *img2 = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSurfaceAlternateImage(src, img2);
    int count = 0;
    SDL_Surface **imgs = SDL_GetSurfaceImages(src, &count);
    printf("images has=%d count=%d same=%d\n", SDL_SurfaceHasAlternateImages(src), count, imgs && imgs[1] == img2);
    SDL_free(imgs);
    SDL_DestroySurface(img2);
    SDL_RemoveSurfaceAlternateImages(src);
    Uint32 key = 1;
    printf("colorkey has=%d get=%d key=%u rle=%d\n", SDL_SurfaceHasColorKey(src), SDL_GetSurfaceColorKey(src, &key), key, SDL_SurfaceHasRLE(src));
    SDL_SetSurfaceRLE(src, true);
    printf("rle=%d cs=%d props=%d\n", SDL_SurfaceHasRLE(src), (int)SDL_GetSurfaceColorspace(src), SDL_GetSurfaceProperties(src) != 0);
    SDL_Surface *sub = SDL_CreateSurface(0, 0, SDL_PIXELFORMAT_ARGB8888);
    printf("zero surface %d pixels=%d\n", sub != NULL, sub && sub->pixels != NULL);
    SDL_DestroySurface(sub);
    SDL_Surface *idx = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_INDEX4LSB);
    SDL_Palette *ip = SDL_CreateSurfacePalette(idx);
    printf("index4 pal=%d n=%d c0=%u,%u,%u\n", ip != NULL, ip ? ip->ncolors : 0, ip ? ip->colors[0].r : 0, ip ? ip->colors[0].g : 0, ip ? ip->colors[0].b : 0);
    SDL_DestroySurface(idx);
    SDL_DestroySurface(src);
    SDL_DestroySurface(dst);
    SDL_DestroyPalette(pal);
}

/* formats wider than 32 bits and colorspace conversion: SDL3 routes these
 * through its floating point blitter (SDL_Blit_Slow_Float) */
static SDL_Surface *make_surface_rgba(SDL_PixelFormat fmt, int w, int h, SDL_Palette *pal)
{
    SDL_Surface *s = SDL_CreateSurface(w, h, fmt);
    if (!s) {
        printf("create %s failed: %s\n", SDL_GetPixelFormatName(fmt), SDL_GetError());
        return NULL;
    }
    if (SDL_ISPIXELFORMAT_INDEXED(fmt)) {
        SDL_SetSurfacePalette(s, pal);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Uint32 v = rnd();
            SDL_WriteSurfacePixel(s, x, y, (Uint8)v, (Uint8)(v >> 8), (Uint8)(v >> 16), (Uint8)(v >> 24));
        }
    }
    SDL_WriteSurfacePixel(s, 0, 0, 255, 255, 255, 255);
    SDL_WriteSurfacePixel(s, 1, 0, 0, 0, 0, 0);
    return s;
}

static void test_wide_and_colorspace(void)
{
    static const SDL_PixelFormat wide[] = {
        SDL_PIXELFORMAT_RGB48, SDL_PIXELFORMAT_RGBA64, SDL_PIXELFORMAT_BGRA64,
        SDL_PIXELFORMAT_RGB48_FLOAT, SDL_PIXELFORMAT_RGBA64_FLOAT,
        SDL_PIXELFORMAT_RGB96_FLOAT, SDL_PIXELFORMAT_RGBA128_FLOAT, SDL_PIXELFORMAT_ARGB128_FLOAT,
        SDL_PIXELFORMAT_XBGR2101010,
    };
    static const SDL_PixelFormat other[] = {
        SDL_PIXELFORMAT_INDEX8, SDL_PIXELFORMAT_RGB565, SDL_PIXELFORMAT_RGB24, SDL_PIXELFORMAT_ARGB8888,
        SDL_PIXELFORMAT_XRGB8888, SDL_PIXELFORMAT_ARGB2101010, SDL_PIXELFORMAT_RGBA64, SDL_PIXELFORMAT_RGBA128_FLOAT,
    };
    static const SDL_BlendMode wblends[] = { SDL_BLENDMODE_NONE, SDL_BLENDMODE_BLEND, SDL_BLENDMODE_ADD, SDL_BLENDMODE_MOD, SDL_BLENDMODE_MUL };
    SDL_Palette *pal = make_palette(256);
    char what[200];

    for (int wi = 0; wi < (int)SDL_arraysize(wide); ++wi) {
        for (int oi = 0; oi < (int)SDL_arraysize(other); ++oi) {
            for (int dir = 0; dir < 2; ++dir) {
                const SDL_PixelFormat sf = dir ? other[oi] : wide[wi];
                const SDL_PixelFormat df = dir ? wide[wi] : other[oi];
                for (int bi = 0; bi < (int)SDL_arraysize(wblends); ++bi) {
                    SDL_Surface *src = make_surface_rgba(sf, 7, 5, pal);
                    SDL_Surface *dst = make_surface_rgba(df, 9, 8, pal);
                    if (!src || !dst) {
                        return;
                    }
                    SDL_SetSurfaceBlendMode(src, wblends[bi]);
                    if (rnd() & 1) {
                        SDL_SetSurfaceColorMod(src, (Uint8)rnd(), (Uint8)rnd(), (Uint8)rnd());
                        SDL_SetSurfaceAlphaMod(src, (Uint8)rnd());
                    }
                    SDL_Rect dr = { 1, 2, 0, 0 };
                    bool ok = SDL_BlitSurface(src, NULL, dst, &dr);
                    snprintf(what, sizeof(what), "wide blit %s->%s %d ok=%d", SDL_GetPixelFormatName(sf), SDL_GetPixelFormatName(df), (int)wblends[bi], ok);
                    out(what, hash_surface(dst));
                    SDL_Rect sdr = { -1, 0, 11, 9 };
                    ok = SDL_BlitSurfaceScaled(src, NULL, dst, &sdr, SDL_SCALEMODE_NEAREST);
                    snprintf(what, sizeof(what), "wide scaled %s->%s %d ok=%d", SDL_GetPixelFormatName(sf), SDL_GetPixelFormatName(df), (int)wblends[bi], ok);
                    out(what, hash_surface(dst));
                    SDL_DestroySurface(src);
                    SDL_DestroySurface(dst);
                }
                SDL_Surface *src = make_surface_rgba(sf, 6, 4, pal);
                SDL_Surface *c = SDL_ConvertSurface(src, df);
                snprintf(what, sizeof(what), "wide convert %s->%s ok=%d", SDL_GetPixelFormatName(sf), SDL_GetPixelFormatName(df), c != NULL);
                out(what, c ? hash_surface(c) : 0);
                SDL_DestroySurface(c);
                SDL_DestroySurface(src);
            }
        }
        SDL_Surface *s = make_surface_rgba(wide[wi], 5, 5, pal);
        float fr, fg, fb, fa;
        Uint8 r, g, b, a;
        SDL_ReadSurfacePixel(s, 2, 2, &r, &g, &b, &a);
        SDL_ReadSurfacePixelFloat(s, 3, 3, &fr, &fg, &fb, &fa);
        printf("wide read %s %u %u %u %u %.5f %.5f %.5f %.5f\n", SDL_GetPixelFormatName(wide[wi]), r, g, b, a, fr, fg, fb, fa);
        SDL_WriteSurfacePixelFloat(s, 0, 0, 0.1f, 1.7f, -0.3f, 0.4f);
        SDL_ReadSurfacePixelFloat(s, 0, 0, &fr, &fg, &fb, &fa);
        printf("wide rw %s %.5f %.5f %.5f %.5f\n", SDL_GetPixelFormatName(wide[wi]), fr, fg, fb, fa);
        for (int lin = 0; lin < 2; ++lin) {
            SDL_Surface *d = SDL_DuplicateSurface(s);
            bool ok = SDL_PremultiplySurfaceAlpha(d, lin != 0);
            snprintf(what, sizeof(what), "wide premul %s lin=%d ok=%d", SDL_GetPixelFormatName(wide[wi]), lin, ok);
            out(what, hash_surface(d));
            SDL_DestroySurface(d);
        }
        SDL_Surface *f = SDL_DuplicateSurface(s);
        bool ok = SDL_FlipSurface(f, SDL_FLIP_HORIZONTAL);
        snprintf(what, sizeof(what), "wide flip %s ok=%d", SDL_GetPixelFormatName(wide[wi]), ok);
        out(what, hash_surface(f));
        ok = SDL_ClearSurface(f, 0.25f, 1.5f, 0.75f, 0.5f);
        snprintf(what, sizeof(what), "wide clear %s ok=%d", SDL_GetPixelFormatName(wide[wi]), ok);
        out(what, hash_surface(f));
        SDL_Surface *sc = SDL_ScaleSurface(s, 8, 3, SDL_SCALEMODE_LINEAR);
        snprintf(what, sizeof(what), "wide scale %s ok=%d", SDL_GetPixelFormatName(wide[wi]), sc != NULL);
        out(what, sc ? hash_surface(sc) : 0);
        SDL_DestroySurface(sc);
        SDL_DestroySurface(f);
        SDL_DestroySurface(s);
    }

    /* explicit colorspaces, SDR white point, HDR headroom, tone mapping */
    static const SDL_Colorspace spaces[] = {
        SDL_COLORSPACE_SRGB, SDL_COLORSPACE_SRGB_LINEAR, SDL_COLORSPACE_HDR10,
    };
    static const char *const tonemaps[] = { NULL, "chrome", "*=0.5", "none" };
    for (int si = 0; si < (int)SDL_arraysize(spaces); ++si) {
        for (int di = 0; di < (int)SDL_arraysize(spaces); ++di) {
            for (int t = 0; t < (int)SDL_arraysize(tonemaps); ++t) {
                SDL_Surface *src = make_surface_rgba(SDL_PIXELFORMAT_RGBA64_FLOAT, 6, 5, pal);
                SDL_Surface *dst = make_surface_rgba(SDL_PIXELFORMAT_ARGB8888, 8, 7, pal);
                SDL_SetSurfaceColorspace(src, spaces[si]);
                SDL_SetSurfaceColorspace(dst, spaces[di]);
                SDL_PropertiesID sp = SDL_GetSurfaceProperties(src);
                SDL_SetFloatProperty(sp, SDL_PROP_SURFACE_SDR_WHITE_POINT_FLOAT, 120.0f + 40.0f * t);
                SDL_SetFloatProperty(sp, SDL_PROP_SURFACE_HDR_HEADROOM_FLOAT, 4.0f);
                if (tonemaps[t]) {
                    SDL_SetStringProperty(sp, SDL_PROP_SURFACE_TONEMAP_OPERATOR_STRING, tonemaps[t]);
                }
                SDL_SetFloatProperty(SDL_GetSurfaceProperties(dst), SDL_PROP_SURFACE_HDR_HEADROOM_FLOAT, t ? 1.5f : 0.0f);
                SDL_SetSurfaceBlendMode(src, (t & 1) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
                bool ok = SDL_BlitSurface(src, NULL, dst, NULL);
                snprintf(what, sizeof(what), "cs blit %x->%x tm=%s ok=%d", (unsigned)spaces[si], (unsigned)spaces[di], tonemaps[t] ? tonemaps[t] : "-", ok);
                out(what, hash_surface(dst));
                SDL_Surface *c = SDL_ConvertSurfaceAndColorspace(src, SDL_PIXELFORMAT_XBGR2101010, NULL, spaces[di], 0);
                snprintf(what, sizeof(what), "cs convert %x->%x tm=%s ok=%d", (unsigned)spaces[si], (unsigned)spaces[di], tonemaps[t] ? tonemaps[t] : "-", c != NULL);
                out(what, c ? hash_surface(c) : 0);
                SDL_DestroySurface(c);
                SDL_DestroySurface(src);
                SDL_DestroySurface(dst);
            }
            Uint8 in[4 * 4 * 4], outb[4 * 4 * 8];
            for (int i = 0; i < (int)sizeof(in); ++i) {
                in[i] = (Uint8)rnd();
            }
            SDL_memset(outb, 0, sizeof(outb));
            bool ok = SDL_ConvertPixelsAndColorspace(4, 4, SDL_PIXELFORMAT_ABGR8888, spaces[si], 0, in, 16,
                                                     SDL_PIXELFORMAT_RGBA64, spaces[di], 0, outb, 32);
            snprintf(what, sizeof(what), "cs convpix %x->%x ok=%d", (unsigned)spaces[si], (unsigned)spaces[di], ok);
            out(what, fnv(0, outb, sizeof(outb)));
        }
    }
    SDL_DestroyPalette(pal);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_pixels();
    test_rects();
    test_blits();
    test_convert();
    test_scale_misc();
    test_wide_and_colorspace();
    printf("done\n");
    return 0;
}
