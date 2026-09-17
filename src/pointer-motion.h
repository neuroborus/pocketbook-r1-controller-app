#ifndef POINTER_MOTION_H
#define POINTER_MOTION_H
#include <stdint.h>

struct pointer_motion {
    uint64_t start, last;
    int sx, sy, rx, ry, valid;
};

static int motion_sign(int n) { return (n > 0) - (n < 0); }

static void pointer_step(struct pointer_motion *m, uint64_t time_ms, int *dx, int *dy)
{
    if (!*dx && !*dy) return;
    int sx = motion_sign(*dx), sy = motion_sign(*dy);
    if (!m->valid || time_ms < m->last || time_ms - m->last >= 220 ||
        (sx && m->sx && sx != m->sx) || (sy && m->sy && sy != m->sy)) {
        *m = (struct pointer_motion){.start = time_ms, .valid = 1};
    }
    uint64_t held = time_ms - m->start;
    if (held > 1800) held = 1800;
    int gain = 3 * 65536 + (int)(12 * 65536 * held / 1800);
    const int divisor = 14 * 65536;
    /* Linear 6..30px/REL28 over 1.8s: no initial dead plateau or late surge.
     * Preserve subpixel fractions instead of quantizing the gain. */
    if (*dx > 128) *dx = 128;
    if (*dx < -128) *dx = -128;
    if (*dy > 128) *dy = 128;
    if (*dy < -128) *dy = -128;
    int ax = *dx * gain + m->rx, ay = *dy * gain + m->ry;
    *dx = ax / divisor; *dy = ay / divisor;
    m->rx = ax % divisor; m->ry = ay % divisor;
    m->last = time_ms;
    /* X/Y arrive in separate reports even during a steady diagonal hold. */
    if (sx) m->sx = sx;
    if (sy) m->sy = sy;
}
#endif
