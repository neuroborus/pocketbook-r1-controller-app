#ifndef TOUCH_MAP_H
#define TOUCH_MAP_H
#include <stdint.h>

/* Map oriented screen coordinates to the baseline's native touch axis ranges.
 * InkView: 0=portrait, 1=90 clockwise, 2=270, 3=180. */
static int touch_map(int sx, int sy, int width, int height, int orientation,
                     int xmin, int xmax, int ymin, int ymax, int *tx, int *ty)
{
    if (width <= 1 || height <= 1 || xmax <= xmin || ymax <= ymin ||
        orientation < 0 || orientation > 3) return -1;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= width) sx = width - 1;
    if (sy >= height) sy = height - 1;
    int nx = sx, ny = sy, dx = width - 1, dy = height - 1;
    if (orientation == 1) { nx = sy; ny = width - 1 - sx; dx = height - 1; dy = width - 1; }
    if (orientation == 2) { nx = height - 1 - sy; ny = sx; dx = height - 1; dy = width - 1; }
    if (orientation == 3) { nx = width - 1 - sx; ny = height - 1 - sy; }
    *tx = xmin + (int)((int64_t)nx * ((int64_t)xmax - xmin) / dx);
    *ty = ymin + (int)((int64_t)ny * ((int64_t)ymax - ymin) / dy);
    return 0;
}
#endif
