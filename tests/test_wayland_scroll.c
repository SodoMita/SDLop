/* Wayland scroll frame semantics, independent of a running compositor.
 * zlib license, see LICENSE. */
#include "../src/video/wayland/sdlop_wayland_scroll.h"
#include <assert.h>
#include <stdio.h>

static void take(SDLOP_WaylandScroll *s, float expected_x, float expected_y)
{
    float x, y;
    SDLOP_WaylandScrollTake(s, &x, &y);
    assert(x == expected_x && y == expected_y);
}
int main(void)
{
    SDLOP_WaylandScroll s = { 0 };
    SDLOP_WaylandScrollContinuous(&s, 1, 1);
    SDLOP_WaylandScrollDiscrete(&s, 1, 1);
    take(&s, 0, -1); /* reported 0.10 detent: discrete overrides fallback */
    take(&s, 0, 0);
    SDLOP_WaylandScrollContinuous(&s, 1, -10);
    SDLOP_WaylandScroll120(&s, 1, -120);
    take(&s, 0, 1); /* paired events, correct sign, not 0 or 2 */
    SDLOP_WaylandScroll120(&s, 0, 30);
    SDLOP_WaylandScroll120(&s, 0, 30);
    SDLOP_WaylandScrollContinuous(&s, 0, 10);
    SDLOP_WaylandScrollDiscrete(&s, 0, 1);
    SDLOP_WaylandScrollContinuous(&s, 1, 5);
    take(&s, 0.5f, -0.5f); /* independent precedence for each axis */
    SDLOP_WaylandScrollContinuous(&s, 0, -20);
    take(&s, -2, 0); /* old protocol / smooth touchpad fallback */
    SDLOP_WaylandScrollDiscrete(&s, 1, -3);
    SDLOP_WaylandScrollContinuous(&s, 1, -1);
    take(&s, 0, 3); /* order-independent */
    puts("PASS: Wayland discrete/value120 precedence, sign, fractions, frame reset");
    return 0;
}
