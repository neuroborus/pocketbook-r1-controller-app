#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "../src/cursor-update-plan.h"

static long total_before, total_after, cases;
static int maximum_requests;
static long total_requests, legacy_requests;
static void check(int ox, int oy, int x, int y, int w, int h)
{
    cursor_rect r[CURSOR_UPDATE_LIMIT], legacy[2];
    int n = cursor_update_plan(ox, oy, x, y, w, h, r);
    assert(n >= 0 && n <= CURSOR_UPDATE_LIMIT);
    int area = 0, old_area = 0;
    int ln = cursor_regions(ox, oy, x, y, w, h, legacy);
    for (int i = 0; i < ln; ++i) old_area += cursor_rect_area(legacy[i]);
    for (int i = 0; i < n; ++i) {
        assert(r[i].x >= 0 && r[i].y >= 0 && r[i].w > 0 && r[i].h > 0);
        assert(r[i].x + r[i].w <= w && r[i].y + r[i].h <= h);
        assert(!(r[i].x % 4) && !(r[i].y % 4));
        assert(!(r[i].w % 4) || r[i].x + r[i].w == w);
        assert(!(r[i].h % 4) || r[i].y + r[i].h == h);
        for (int j = 0; j < i; ++j) assert(!cursor_rect_overlap(r[i], r[j]));
        area += cursor_rect_area(r[i]);
    }
    assert(area <= old_area);
    /* All changed pixels in BOTH footprints must be covered. */
    for (int pass = 0; pass < 2; ++pass)
        for (int sy = 0; sy < CURSOR_H; ++sy)
            for (int sx = 0; sx < CURSOR_W; ++sx) {
                int px = (pass ? x : ox) + sx, py = (pass ? y : oy) + sy;
                if (cursor_mask_at(px, py, ox, oy) == cursor_mask_at(px, py, x, y)) continue;
                int covered = 0;
                for (int i = 0; i < n; ++i)
                    covered |= px >= r[i].x && py >= r[i].y && px < r[i].x+r[i].w && py < r[i].y+r[i].h;
                assert(covered);
            }
    total_before += old_area; total_after += area; ++cases;
    total_requests += n; legacy_requests += ln;
    if (n > maximum_requests) maximum_requests = n;
}

int main(int argc, char **argv)
{
    cursor_init_mask();
    /* Reference functions exercised elsewhere; also ensure this header uses
     * the actual same sprite on gray background, not an invented rectangle. */
    uint8_t pixels[96 * 100 * 3], saved[sizeof(pixels)];
    memset(pixels, 127, sizeof(pixels));
    cursor_rect sample = {0,0,24,32};
    assert(cursor_needs_gray(pixels, 96*3, sample));
    memcpy(saved, pixels, sizeof(pixels));
    cursor_restore(pixels, saved, 96*3, sample);
    cursor_draw(pixels, 96*3, 0, 0);
    if (argc == 2) {
        struct timespec started, finished;
        assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
        FILE *f = fopen(argv[1], "r");
        assert(f);
        char line[512];
        int ox = 0, oy = 0, seen = 0, x, y;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "FRAME x=%d y=%d", &x, &y) != 2) continue;
            if (seen) check(ox, oy, x, y, 1264, 1680);
            ox = x; oy = y; seen = 1;
        }
        fclose(f);
        assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
        assert(cases > 0);
        printf("REPLAY: %ld moves area_before=%ld area_after=%ld reduction=%.1f%% calls_before=%ld calls_after=%ld max_calls=%d\n",
            cases,total_before,total_after,100.0*(total_before-total_after)/total_before,
            legacy_requests,total_requests,maximum_requests);
        double elapsed = (finished.tv_sec - started.tv_sec) * 1000.0 +
            (finished.tv_nsec - started.tv_nsec) / 1000000.0;
        printf("REPLAY_CPU with assertions: total_ms=%.3f ms_per_move=%.3f\n", elapsed, elapsed/cases);
        return 0;
    }
    for (int ax = 0; ax < 4; ++ax)
        for (int ay = 0; ay < 4; ++ay)
            for (int dx = -40; dx <= 40; dx += 2)
                for (int dy = -40; dy <= 40; dy += 2)
                    check(100+ax,100+ay,100+ax+dx,100+ay+dy,1264,1680);
    check(0,0,1239,1647,1263,1679);
    check(1239,1647,0,0,1263,1679);
    check(0,0,73,69,97,101);
    check(73,69,72,68,97,101);
    printf("PASS: %ld cases, coverage/alignment/no overlap/area bound, max_requests=%d area_reduction=%.1f%%\n",
        cases, maximum_requests, 100.0*(total_before-total_after)/total_before);
}
