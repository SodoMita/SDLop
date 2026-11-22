/* Wayland axis frame arbitration. Protocol values, no Wayland dependency.
 * zlib license, see LICENSE. */
#ifndef SDLOP_WAYLAND_SCROLL_H
#define SDLOP_WAYLAND_SCROLL_H

typedef struct SDLOP_WaylandScroll
{
    float continuous[2], discrete[2], value120[2]; /* horizontal, vertical */
    unsigned discrete_axes, value120_axes;
} SDLOP_WaylandScroll;

static inline void SDLOP_WaylandScrollContinuous(SDLOP_WaylandScroll *s, int axis, float value)
{
    s->continuous[axis] += value / 10.0f; /* fallback surface-distance units */
}
static inline void SDLOP_WaylandScrollDiscrete(SDLOP_WaylandScroll *s, int axis, int value)
{
    s->discrete_axes |= 1u << axis;
    s->discrete[axis] += (float)value;
}
static inline void SDLOP_WaylandScroll120(SDLOP_WaylandScroll *s, int axis, int value)
{
    s->value120_axes |= 1u << axis;
    s->value120[axis] += (float)value / 120.0f;
}
static inline void SDLOP_WaylandScrollTake(SDLOP_WaylandScroll *s, float *x, float *y)
{
    float result[2];
    for (int axis = 0; axis < 2; axis++) {
        unsigned bit = 1u << axis;
        result[axis] = (s->value120_axes & bit) ? s->value120[axis] :
                       (s->discrete_axes & bit) ? s->discrete[axis] : s->continuous[axis];
    }
    *x = result[0];
    *y = -result[1]; /* Wayland down-positive -> SDL up-positive, for all sources */
    *s = (SDLOP_WaylandScroll){ 0 };
}
#endif
