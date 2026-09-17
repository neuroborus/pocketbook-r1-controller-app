#ifndef CURSOR_REPAIR_SEED_H
#define CURSOR_REPAIR_SEED_H
#include "cursor-repair.h"
/* Change only exact debt pixels with a non-binary CURRENT target. Pure BW
 * pixels (including the visible current arrow) remain untouched. A temporary
 * endpoint gives the controller a real BW->gray transition for the second
 * pass, instead of requesting gray->same-gray after DU left an optical ghost.
 * This is an experimental controller-state hypothesis, not a panel guarantee. */
static size_t cursor_repair_seed(const struct cursor_repair *p,uint8_t *rgb,int stride,cursor_rect r)
{
    size_t changed=0;
    for (int y=r.y;y<r.y+r.h;++y)
        for (int x=r.x;x<r.x+r.w;++x) {
            if (!cursor_repair_pixel(p,x,y)) continue;
            uint8_t *v=rgb+(size_t)y*(size_t)stride+(size_t)x*3;
            if ((v[0]==0 && v[1]==0 && v[2]==0) || (v[0]==255 && v[1]==255 && v[2]==255)) continue;
            uint8_t endpoint=(unsigned)v[0]+v[1]+v[2]>=384?255:0;
            v[0]=v[1]=v[2]=endpoint; ++changed;
        }
    return changed;
}
#endif
