#ifndef CURSOR_UPDATE_PLAN_H
#define CURSOR_UPDATE_PLAN_H
#include <stdlib.h>
#include "cursor-raster.h"

/* UpdateEx rounds outward to 4px. Track changed silhouette tiles, not the
 * rectangle swept by the pointer. Cap requests to avoid a long hardware queue. */
#define CURSOR_UPDATE_LIMIT 4
#define CURSOR_TILE_LIMIT 256

static int cursor_mask_at(int px, int py, int x, int y)
{
    int sx = px - x, sy = py - y;
    return sx >= 0 && sx < CURSOR_W && sy >= 0 && sy < CURSOR_H ? cursor_mask[sy][sx] : 0;
}

static int cursor_rect_area(cursor_rect r) { return r.w * r.h; }
static cursor_rect cursor_rect_union(cursor_rect a, cursor_rect b)
{
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (cursor_rect){x, y, right - x, bottom - y};
}
static int cursor_rect_overlap(cursor_rect a, cursor_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}
static int cursor_tile_order(const void *ap, const void *bp)
{
    const cursor_rect *a = ap, *b = bp;
    return a->y != b->y ? a->y - b->y : a->x - b->x;
}
static void cursor_remove_rect(cursor_rect *r, int *n, int i)
{
    memmove(r + i, r + i + 1, (size_t)(*n - i - 1) * sizeof(*r));
    --*n;
}

static int cursor_update_plan(int ox, int oy, int x, int y, int w, int h,
                              cursor_rect out[CURSOR_UPDATE_LIMIT])
{
    cursor_rect tiles[CURSOR_TILE_LIMIT];
    int n = 0;
    for (int pass = 0; pass < 2; ++pass) {
        int base_x = pass ? x : ox, base_y = pass ? y : oy;
        for (int sy = 0; sy < CURSOR_H; ++sy)
            for (int sx = 0; sx < CURSOR_W; ++sx) {
                int px = base_x + sx, py = base_y + sy;
                if (px < 0 || py < 0 || px >= w || py >= h ||
                    cursor_mask_at(px, py, ox, oy) == cursor_mask_at(px, py, x, y)) continue;
                int tx = px & ~3, ty = py & ~3, found = 0;
                for (int i = 0; i < n; ++i)
                    if (tiles[i].x == tx && tiles[i].y == ty) { found = 1; break; }
                if (found) continue;
                /* Two 24x32 footprints touch at most 2*7*9=126 tiles. */
                if (n == CURSOR_TILE_LIMIT) return -1;
                tiles[n++] = (cursor_rect){tx, ty, tx + 4 <= w ? 4 : w - tx,
                    ty + 4 <= h ? 4 : h - ty};
            }
    }
    qsort(tiles, (size_t)n, sizeof(*tiles), cursor_tile_order);
    /* First coalesce without adding any unchanged tiles. */
    for (int i = 0; i + 1 < n; ) {
        if (tiles[i].y == tiles[i+1].y && tiles[i].h == tiles[i+1].h &&
            tiles[i].x + tiles[i].w == tiles[i+1].x) {
            tiles[i].w += tiles[i+1].w;
            cursor_remove_rect(tiles, &n, i + 1);
        } else ++i;
    }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ) {
            if (tiles[i].x == tiles[j].x && tiles[i].w == tiles[j].w &&
                tiles[i].y + tiles[i].h == tiles[j].y) {
                tiles[i].h += tiles[j].h;
                cursor_remove_rect(tiles, &n, j);
            } else ++j;
        }
    /* Greedily add the least background until the request budget is met. */
    while (n > CURSOR_UPDATE_LIMIT) {
        int ai = 0, bi = 1, best = INT32_MAX;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                int cost = cursor_rect_area(cursor_rect_union(tiles[i], tiles[j])) -
                    cursor_rect_area(tiles[i]) - cursor_rect_area(tiles[j]);
                if (cost < best) { best = cost; ai = i; bi = j; }
            }
        tiles[ai] = cursor_rect_union(tiles[ai], tiles[bi]);
        cursor_remove_rect(tiles, &n, bi);
        /* Never submit overlapping requests in one frame. */
        int again = 1;
        while (again) {
            again = 0;
            for (int i = 0; i < n && !again; ++i)
                for (int j = i + 1; j < n; ++j)
                    if (cursor_rect_overlap(tiles[i], tiles[j])) {
                        tiles[i] = cursor_rect_union(tiles[i], tiles[j]);
                        cursor_remove_rect(tiles, &n, j);
                        again = 1;
                        break;
                    }
        }
    }
    /* Never exceed the two-footprint area: fall back if greedy merging lost. */
    cursor_rect legacy[2];
    int legacy_n = cursor_regions(ox, oy, x, y, w, h, legacy), area = 0, legacy_area = 0;
    for (int i = 0; i < n; ++i) area += cursor_rect_area(tiles[i]);
    for (int i = 0; i < legacy_n; ++i) legacy_area += cursor_rect_area(legacy[i]);
    if (area >= legacy_area) { memcpy(out, legacy, (size_t)legacy_n * sizeof(*out)); return legacy_n; }
    memcpy(out, tiles, (size_t)n * sizeof(*out));
    return n;
}
#endif
