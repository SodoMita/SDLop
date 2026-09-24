/*
  SDLop -- SDL_rect.h.

  Rectangle intersection/union helpers. These are the ten functions SDL3 exposes;
  the floating point variants are generated from the same code by the macro below,
  so the two families cannot drift apart.

  All of them use the "half open" convention SDL3 documents: a rect covers
  [x, x+w) and edges that only touch do not count as an intersection.
*/

#include "../sdlop_internal.h"

#define SDLOP_DEFINE_RECT_FUNCS(SUFFIX, T, POINT, RECT)                                       \
    bool SDL_HasRectIntersection##SUFFIX(const RECT *A, const RECT *B)                            \
    {                                                                                          \
        T Amin, Amax, Bmin, Bmax;                                                              \
        if (!A || !B) {                                                                        \
            return false;                                                                      \
        }                                                                                      \
        if (A->w <= 0 || A->h <= 0 || B->w <= 0 || B->h <= 0) {                                \
            return false;                                                                      \
        }                                                                                      \
        Amin = A->x; Amax = A->x + A->w;                                                       \
        Bmin = B->x; Bmax = B->x + B->w;                                                       \
        if (Bmax <= Amin || Amax <= Bmin) {                                                    \
            return false;                                                                      \
        }                                                                                      \
        Amin = A->y; Amax = A->y + A->h;                                                       \
        Bmin = B->y; Bmax = B->y + B->h;                                                       \
        if (Bmax <= Amin || Amax <= Bmin) {                                                    \
            return false;                                                                      \
        }                                                                                      \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    bool SDL_GetRectIntersection##SUFFIX(const RECT *A, const RECT *B, RECT *result)              \
    {                                                                                          \
        T Amin, Amax, Bmin, Bmax;                                                              \
        if (!result || !A || !B) {                                                             \
            return false;                                                                      \
        }                                                                                      \
        Amin = A->x; Amax = A->x + A->w;                                                       \
        Bmin = B->x; Bmax = B->x + B->w;                                                       \
        if (Bmax <= Amin || Amax <= Bmin) {                                                    \
            result->x = 0; result->y = 0; result->w = 0; result->h = 0;                        \
            return false;                                                                      \
        }                                                                                      \
        result->x = Amin > Bmin ? Amin : Bmin;                                                 \
        result->w = (Amax < Bmax ? Amax : Bmax) - result->x;                                   \
        Amin = A->y; Amax = A->y + A->h;                                                       \
        Bmin = B->y; Bmax = B->y + B->h;                                                       \
        if (Bmax <= Amin || Amax <= Bmin) {                                                    \
            result->x = 0; result->y = 0; result->w = 0; result->h = 0;                        \
            return false;                                                                      \
        }                                                                                      \
        result->y = Amin > Bmin ? Amin : Bmin;                                                 \
        result->h = (Amax < Bmax ? Amax : Bmax) - result->y;                                   \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    bool SDL_GetRectUnion##SUFFIX(const RECT *A, const RECT *B, RECT *result)                     \
    {                                                                                          \
        if (!result || !A || !B) {                                                             \
            return false;                                                                      \
        }                                                                                      \
        if (A->w <= 0 || A->h <= 0) {                                                          \
            *result = *B;                                                                      \
            return true;                                                                       \
        }                                                                                      \
        if (B->w <= 0 || B->h <= 0) {                                                          \
            *result = *A;                                                                      \
            return true;                                                                       \
        }                                                                                      \
        {                                                                                      \
            T Amin = A->x < B->x ? A->x : B->x;                                                \
            T Amax = (A->x + A->w) > (B->x + B->w) ? (A->x + A->w) : (B->x + B->w);            \
            T Bmin = A->y < B->y ? A->y : B->y;                                                \
            T Bmax = (A->y + A->h) > (B->y + B->h) ? (A->y + A->h) : (B->y + B->h);            \
            result->x = Amin;                                                                  \
            result->w = Amax - Amin;                                                           \
            result->y = Bmin;                                                                  \
            result->h = Bmax - Bmin;                                                           \
        }                                                                                      \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    bool SDL_GetRectEnclosingPoints##SUFFIX(const POINT *points, int count, const RECT *clip,     \
                                         RECT *result)                                         \
    {                                                                                          \
        T minx, miny, maxx, maxy;                                                              \
        int i;                                                                                 \
        if (!points || !result || count < 1) {                                                 \
            return false;                                                                      \
        }                                                                                      \
        minx = maxx = points[0].x;                                                             \
        miny = maxy = points[0].y;                                                             \
        for (i = 1; i < count; i++) {                                                          \
            minx = points[i].x < minx ? points[i].x : minx;                                    \
            miny = points[i].y < miny ? points[i].y : miny;                                    \
            maxx = points[i].x > maxx ? points[i].x : maxx;                                    \
            maxy = points[i].y > maxy ? points[i].y : maxy;                                    \
        }                                                                                      \
        result->x = minx;                                                                      \
        result->y = miny;                                                                      \
        result->w = maxx - minx;                                                               \
        result->h = maxy - miny;                                                               \
        if (clip && !SDL_GetRectIntersection##SUFFIX(result, clip, result)) {                     \
            return false;                                                                      \
        }                                                                                      \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    bool SDL_GetRectAndLineIntersection##SUFFIX(const RECT *rect, T *X1, T *Y1, T *X2, T *Y2)   \
    {                                                                                          \
        T x1, y1, x2, y2;                                                                      \
        T left, right, top, bottom;                                                            \
        if (!rect || !X1 || !Y1 || !X2 || !Y2) {                                               \
            return false;                                                                      \
        }                                                                                      \
        x1 = *X1; y1 = *Y1;                                                                    \
        x2 = *X2; y2 = *Y2;                                                                    \
        left = rect->x; right = rect->x + rect->w;                                             \
        top = rect->y; bottom = rect->y + rect->h;                                             \
        if (x1 == x2 && y1 == y2) {                                                            \
            return false;                                                                      \
        }                                                                                      \
        {                                                                                      \
            /* Liang-Barsky: clip the segment against the four edges */                        \
            T dx = x2 - x1, dy = y2 - y1;                                                      \
            T t0 = 0.0, t1 = 1.0;                                                              \
            const T p[4] = { -dx, dx, -dy, dy };                                               \
            const T q[4] = { x1 - left, right - x1, y1 - top, bottom - y1 };                   \
            for (int i = 0; i < 4; i++) {                                                      \
                if (p[i] == 0) {                                                               \
                    if (q[i] < 0) {                                                            \
                        return false;                                                          \
                    }                                                                          \
                } else {                                                                       \
                    T r = q[i] / p[i];                                                         \
                    if (p[i] < 0) {                                                            \
                        if (r > t1) return false;                                              \
                        if (r > t0) t0 = r;                                                    \
                    } else {                                                                   \
                        if (r < t0) return false;                                              \
                        if (r < t1) t1 = r;                                                    \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
            *X1 = x1 + t0 * dx; *Y1 = y1 + t0 * dy;                                            \
            *X2 = x1 + t1 * dx; *Y2 = y1 + t1 * dy;                                            \
        }                                                                                      \
        return true;                                                                           \
    }

SDLOP_DEFINE_RECT_FUNCS(, int, SDL_Point, SDL_Rect)
SDLOP_DEFINE_RECT_FUNCS(Float, float, SDL_FPoint, SDL_FRect)

#undef SDLOP_DEFINE_RECT_FUNCS
