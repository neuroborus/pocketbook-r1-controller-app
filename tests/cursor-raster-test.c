#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/cursor-raster.h"

static uint32_t rng = 17;
static unsigned random_value(void)
{
    rng = rng * 1664525U + 1013904223U;
    return rng;
}

static void exercise(int w, int h)
{
    const int stride = w * 3 + 13; /* Deliberately padded and unaligned rows. */
    size_t size = (size_t)stride * (size_t)h;
    uint8_t *saved = malloc(size), *frame = malloc(size);
    assert(saved && frame);
    for (size_t i = 0; i < size; ++i) saved[i] = (uint8_t)random_value();
    memcpy(frame, saved, size);
    int old_x = 0, old_y = 0;
    cursor_draw(frame, stride, old_x, old_y);
    for (int iteration = 0; iteration < 200; ++iteration) {
        int x = (int)(random_value() % (unsigned)(w - CURSOR_W + 1));
        int y = (int)(random_value() % (unsigned)(h - CURSOR_H + 1));
        if (iteration % 4 == 0) x = 0;
        if (iteration % 4 == 1) x = w - CURSOR_W;
        if (iteration % 4 == 2) y = 0;
        if (iteration % 4 == 3) y = h - CURSOR_H;
        cursor_rect rect = cursor_dirty(old_x, old_y, x, y, w, h);
        assert(rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= w && rect.y + rect.h <= h);
        cursor_restore(frame, saved, stride, rect);
        /* No former cursor pixels may survive anywhere in the whole frame. */
        assert(memcmp(frame, saved, size) == 0);
        cursor_draw(frame, stride, x, y);
        for (int py = 0; py < h; ++py)
            for (int byte = 0; byte < stride; ++byte) {
                int px = byte / 3;
                int sx = px - x, sy = py - y;
                if (px < w && sx >= 0 && sx < CURSOR_W && sy >= 0 && sy < CURSOR_H && cursor_mask[sy][sx])
                    continue;
                size_t offset = (size_t)py * (size_t)stride + (size_t)byte;
                assert(frame[offset] == saved[offset]);
            }
        old_x = x; old_y = y;
    }
    cursor_restore(frame, saved, stride, cursor_dirty(old_x, old_y, old_x, old_y, w, h));
    assert(memcmp(frame, saved, size) == 0);
    printf("PASS %dx%d: 200 movements; no trails in memory; transparent pixels and row padding intact\n", w, h);
    free(saved);
    free(frame);
}

int main(void)
{
    cursor_rect regions[2];
    assert(cursor_regions(0, 0, 800, 1000, 1264, 1680, regions) == 2);
    assert(regions[0].w * regions[0].h + regions[1].w * regions[1].h == 2 * 24 * 32);
    assert(cursor_regions(0, 0, 4, 4, 1264, 1680, regions) == 1);
    assert(regions[0].w == 28 && regions[0].h == 36);
    uint8_t gray[8 * 8 * 3];
    cursor_rect all = {0, 0, 8, 8};
    memset(gray, 0, sizeof(gray));
    assert(!cursor_needs_gray(gray, 24, all));
    memset(gray, 255, sizeof(gray));
    assert(!cursor_needs_gray(gray, 24, all));
    memset(gray, 128, 8 * 3);
    assert(!cursor_needs_gray(gray, 24, all)); /* exactly 1/8 */
    gray[8 * 3] = 100;
    assert(cursor_needs_gray(gray, 24, all));
    assert(!cursor_needs_gray(gray, 24, (cursor_rect){0, 4, 8, 4}));
    memset(gray, 128, sizeof(gray));
    assert(cursor_needs_gray(gray, 24, all));
    cursor_init_mask();
    int transparent = 0, light = 0, dark = 0;
    for (int y = 0; y < CURSOR_H; ++y)
        for (int x = 0; x < CURSOR_W; ++x) {
            transparent += cursor_mask[y][x] == 0;
            light += cursor_mask[y][x] == 1;
            dark += cursor_mask[y][x] == 2;
        }
    assert(transparent > CURSOR_W * CURSOR_H / 2 && light > 0 && dark > 0);
    printf("SPRITE transparent=%d outline=%d fill=%d\n", transparent, light, dark);
    exercise(97, 101);
    exercise(128, 192);
    exercise(1264, 1680);
    puts("PASS: bounded two-region large jumps, overlapping footprints, BW/grayscale classification");
    return 0;
}
