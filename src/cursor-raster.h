#ifndef CURSOR_RASTER_H
#define CURSOR_RASTER_H

#include <stdint.h>
#include <string.h>

#define CURSOR_W 24
#define CURSOR_H 32

typedef struct { int x, y, w, h; } cursor_rect;

/* An arrow with a one-pixel light outline. Zero mask leaves background intact. */
static uint8_t cursor_mask[CURSOR_H][CURSOR_W];

static void cursor_init_mask(void)
{
    const int vx[] = {2, 2, 8, 13, 17, 12, 22};
    const int vy[] = {2, 25, 19, 30, 28, 17, 17};
    uint8_t inside[CURSOR_H][CURSOR_W] = {{0}};
    memset(cursor_mask, 0, sizeof(cursor_mask));
    for (int y = 0; y < CURSOR_H; ++y)
        for (int x = 0; x < CURSOR_W; ++x) {
            int hit = 0;
            for (int i = 0, j = 6; i < 7; j = i++)
                if ((vy[i] > y) != (vy[j] > y) &&
                    x < vx[i] + (double)(vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]))
                    hit = !hit;
            inside[y][x] = (uint8_t)hit;
        }
    for (int y = 0; y < CURSOR_H; ++y)
        for (int x = 0; x < CURSOR_W; ++x) {
            if (!inside[y][x]) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && x + dx < CURSOR_W && y + dy >= 0 && y + dy < CURSOR_H)
                        cursor_mask[y + dy][x + dx] = 1;
        }
    for (int y = 0; y < CURSOR_H; ++y)
        for (int x = 0; x < CURSOR_W; ++x)
            if (inside[y][x]) cursor_mask[y][x] = 2;
}

static cursor_rect cursor_dirty(int old_x, int old_y, int x, int y, int width, int height)
{
    int left = (old_x < x ? old_x : x) & ~3;
    int top = (old_y < y ? old_y : y) & ~3;
    int right = ((old_x > x ? old_x : x) + CURSOR_W + 3) & ~3;
    int bottom = ((old_y > y ? old_y : y) + CURSOR_H + 3) & ~3;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    return (cursor_rect){left, top, right - left, bottom - top};
}

/* Large jumps need only the two cursor footprints, never their whole path. */
static int cursor_regions(int ox, int oy, int x, int y, int w, int h, cursor_rect out[2])
{
    out[0] = cursor_dirty(ox, oy, ox, oy, w, h);
    out[1] = cursor_dirty(x, y, x, y, w, h);
    if (out[0].x + out[0].w <= out[1].x || out[1].x + out[1].w <= out[0].x ||
        out[0].y + out[0].h <= out[1].y || out[1].y + out[1].h <= out[0].y) return 2;
    out[0] = cursor_dirty(ox, oy, x, y, w, h);
    return 1;
}

/* BW updates quantize menu artwork: use grayscale for areas with >1/8
 * non-black/white background pixels, without a later flashing idle cleanup. */
static int cursor_needs_gray(const uint8_t *saved, int stride, cursor_rect r)
{
    size_t count = 0, limit = (size_t)r.w * (size_t)r.h / 8;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint8_t *p = saved + (size_t)y * (size_t)stride + (size_t)x * 3;
            int black = p[0] <= 16 && p[1] <= 16 && p[2] <= 16;
            int white = p[0] >= 239 && p[1] >= 239 && p[2] >= 239;
            if (!black && !white && ++count > limit) return 1;
        }
    return 0;
}

static void cursor_restore(uint8_t *dst, const uint8_t *saved, int stride, cursor_rect r)
{
    for (int y = r.y; y < r.y + r.h; ++y) {
        size_t offset = (size_t)y * (size_t)stride + (size_t)r.x * 3;
        memcpy(dst + offset, saved + offset, (size_t)r.w * 3);
    }
}

static void cursor_draw(uint8_t *dst, int stride, int x, int y)
{
    for (int sy = 0; sy < CURSOR_H; ++sy)
        for (int sx = 0; sx < CURSOR_W; ++sx) {
            uint8_t mask = cursor_mask[sy][sx];
            if (!mask) continue;
            size_t offset = (size_t)(y + sy) * (size_t)stride + (size_t)(x + sx) * 3;
            memset(dst + offset, mask == 1 ? 255 : 0, 3);
        }
}

#endif
