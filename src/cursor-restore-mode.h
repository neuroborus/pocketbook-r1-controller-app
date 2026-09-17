#ifndef CURSOR_RESTORE_MODE_H
#define CURSOR_RESTORE_MODE_H
#include "cursor-update-plan.h"

/* DU can draw the binary arrow but cannot reliably restore intermediate
 * image tones from a former black/white cursor pixel. Request gray only where
 * an old opaque pixel is exposed and the saved background is not pure BW. */
static int cursor_restore_needs_gray(const uint8_t *saved, int stride,
                                     int ox,int oy,int x,int y,cursor_rect r)
{
    int left = r.x > ox ? r.x : ox, top = r.y > oy ? r.y : oy;
    int right = r.x+r.w < ox+CURSOR_W ? r.x+r.w : ox+CURSOR_W;
    int bottom = r.y+r.h < oy+CURSOR_H ? r.y+r.h : oy+CURSOR_H;
    for (int py=top;py<bottom;++py)
        for (int px=left;px<right;++px) {
            if (!cursor_mask_at(px,py,ox,oy) || cursor_mask_at(px,py,x,y)) continue;
            const uint8_t *p = saved+(size_t)py*(size_t)stride+(size_t)px*3;
            if (!((p[0]==0 && p[1]==0 && p[2]==0) ||
                  (p[0]==255 && p[1]==255 && p[2]==255))) return 1;
        }
    return 0;
}
#endif
