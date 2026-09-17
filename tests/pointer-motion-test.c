#include <assert.h>
#include <stdio.h>
#include "../src/pointer-motion.h"

static void step(struct pointer_motion *m, uint64_t t, int x, int y, int ex, int ey)
{
    pointer_step(m, t, &x, &y);
    assert(x == ex && y == ey);
}

int main(void)
{
    struct pointer_motion m = {0};
    double total = 0;
    int actual = 0, previous = 6;
    for (unsigned t = 0; t <= 2200; t += 100) {
        int x = 28, y = 28;
        pointer_step(&m, 1000 + t, &x, &y);
        total += 6 + 24.0 * (t < 1800 ? t : 1800) / 1800;
        actual += x;
        assert(x == y && x >= 6 && x <= 30);
        assert(x >= previous && x - previous <= 2);
        assert(total - actual >= -0.001 && total - actual < 1.001);
        if (t == 500) assert(x >= 12 && x <= 13);
        if (t >= 1800) assert(x == 30);
        previous = x;
    }
    step(&m, 3300, -28, 0, -6, 0);
    step(&m, 3520, -28, 0, -6, 0);
    step(&m, 3500, 28, 28, 6, 6);
    m = (struct pointer_motion){0};
    for (unsigned t = 0; t <= 2000; t += 100) {
        int x = -28, y = 0;
        pointer_step(&m, t, &x, &y);
        assert(m.start == 0 && x <= -6 && y == 0);
        x = 0; y = -28;
        pointer_step(&m, t + 15, &x, &y);
        assert(m.start == 0 && x == 0 && y <= -6);
    }
    step(&m, 2050, 28, 0, 6, 0);
    m = (struct pointer_motion){0};
    for (unsigned t = 0; t < 4; ++t) step(&m, t, 1, 0, 0, 0);
    step(&m, 4, 1, 0, 1, 0);
    m = (struct pointer_motion){0};
    step(&m, 0, 0, 0, 0, 0);
    assert(!m.valid);
    int x = INT32_MAX, y = INT32_MIN;
    pointer_step(&m, 1, &x, &y);
    assert(x >= 0 && x <= 28 && y <= 0 && y >= -28);
    puts("PASS: linear 6..30px/1.8s, max +2px per 100ms, subpixel conservation, split axes, resets, malformed deltas");
}
