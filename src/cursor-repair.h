#ifndef CURSOR_REPAIR_H
#define CURSOR_REPAIR_H
#include "cursor-update-plan.h"
#define CURSOR_REPAIR_LIMIT 4
#define CURSOR_REPAIR_IDLE_MS 250
#define CURSOR_REPAIR_OUTPUT_LIMIT 8
#define REPAIR_COLS (1264/4)
#define REPAIR_ROWS (1680/4)
struct cursor_repair {
    cursor_rect rects[CURSOR_REPAIR_LIMIT+1];
    int count;
    uint64_t last_activity;
    uint8_t tiles[(REPAIR_COLS*REPAIR_ROWS+7)/8];
    uint8_t pixels[(REPAIR_COLS*REPAIR_ROWS*16+7)/8];
};
static void cursor_repair_bound(struct cursor_repair *p,cursor_rect r)
{
    if (r.w<=0 || r.h<=0) return;
    p->rects[p->count++]=r;
    for (;;) {
        int ai=-1,bi=-1;
        for (int i=0;i<p->count && ai<0;++i)
            for (int j=i+1;j<p->count;++j)
                if (cursor_rect_overlap(p->rects[i],p->rects[j])) { ai=i; bi=j; break; }
        if (ai<0 && p->count<=CURSOR_REPAIR_LIMIT) break;
        if (ai<0) {
            int best=INT32_MAX;
            for (int i=0;i<p->count;++i)
                for (int j=i+1;j<p->count;++j) {
                    int cost=cursor_rect_area(cursor_rect_union(p->rects[i],p->rects[j]))-
                        cursor_rect_area(p->rects[i])-cursor_rect_area(p->rects[j]);
                    if (cost<best) { best=cost; ai=i; bi=j; }
                }
        }
        p->rects[ai]=cursor_rect_union(p->rects[ai],p->rects[bi]);
        cursor_remove_rect(p->rects,&p->count,bi);
    }
}
static void cursor_repair_mark(struct cursor_repair *p,int x,int y)
{
    if (x<0 || y<0 || x>=REPAIR_COLS*4 || y>=REPAIR_ROWS*4) return;
    unsigned i=(unsigned)(y/4*REPAIR_COLS+x/4);
    p->tiles[i/8]|=(uint8_t)(1U<<(i%8));
    i=(unsigned)(y*(REPAIR_COLS*4)+x);
    p->pixels[i/8]|=(uint8_t)(1U<<(i%8));
}
static inline int cursor_repair_pixel(const struct cursor_repair *p,int x,int y)
{
    unsigned i=(unsigned)(y*(REPAIR_COLS*4)+x);
    return (p->pixels[i/8]>>(i%8))&1;
}
static int cursor_repair_marked(const struct cursor_repair *p,int x,int y)
{
    unsigned i=(unsigned)(y*REPAIR_COLS+x);
    return (p->tiles[i/8]>>(i%8))&1;
}
static inline void cursor_repair_add(struct cursor_repair *p,cursor_rect r)
{
    for (int y=r.y;y<r.y+r.h;++y)
        for (int x=r.x;x<r.x+r.w;++x) cursor_repair_mark(p,x,y);
    cursor_repair_bound(p,r);
}
static inline void cursor_repair_hide(struct cursor_repair *p,int x,int y,cursor_rect r)
{
    for (int sy=0;sy<CURSOR_H;++sy)
        for (int sx=0;sx<CURSOR_W;++sx)
            if (cursor_mask[sy][sx]) cursor_repair_mark(p,x+sx,y+sy);
    cursor_repair_bound(p,r);
}
/* Track only old opaque pixels exposed over non-binary background. Preserve
 * four-rectangle bound as a safe starting plan, then tighten at idle. */
static int cursor_repair_exposed(struct cursor_repair *p,const uint8_t *saved,int stride,
                                 int ox,int oy,int x,int y,cursor_rect r)
{
    int found=0;
    for (int sy=0;sy<CURSOR_H;++sy)
        for (int sx=0;sx<CURSOR_W;++sx) {
            int px=ox+sx,py=oy+sy;
            if (px<r.x || py<r.y || px>=r.x+r.w || py>=r.y+r.h ||
                !cursor_mask[sy][sx] || cursor_mask_at(px,py,x,y)) continue;
            const uint8_t *v=saved+(size_t)py*(size_t)stride+(size_t)px*3;
            if ((v[0]==0 && v[1]==0 && v[2]==0) || (v[0]==255 && v[1]==255 && v[2]==255)) continue;
            cursor_repair_mark(p,px,py); found=1;
        }
    if (found) cursor_repair_bound(p,r);
    return found;
}
static cursor_rect repair_join(cursor_rect a,cursor_rect b)
{
    if (!a.w) return b;
    if (!b.w) return a;
    return cursor_rect_union(a,b);
}
/* Evaluate every horizontal/vertical guillotine cut using slice prefix/suffix
 * bounds: linear in covered tiles, not a pixel scan per candidate cut. */
static int cursor_repair_split(const struct cursor_repair *p,cursor_rect r,
                              cursor_rect *tight,cursor_rect pair[2])
{
    cursor_rect rows[REPAIR_ROWS]={0},cols[REPAIR_COLS]={0};
    *tight=(cursor_rect){0};
    for (int y=r.y/4;y<(r.y+r.h+3)/4;++y)
        for (int x=r.x/4;x<(r.x+r.w+3)/4;++x) {
            if (!cursor_repair_marked(p,x,y)) continue;
            cursor_rect t={x*4,y*4,4,4};
            rows[y]=repair_join(rows[y],t); cols[x]=repair_join(cols[x],t);
            *tight=repair_join(*tight,t);
        }
    int best=0;
    for (int axis=0;axis<2;++axis) {
        cursor_rect *s=axis?rows:cols;
        int n=axis?REPAIR_ROWS:REPAIR_COLS;
        cursor_rect suffix[REPAIR_ROWS+1]={0},left={0};
        for (int i=n-1;i>=0;--i) suffix[i]=repair_join(s[i],suffix[i+1]);
        for (int i=0;i<n-1;++i) {
            left=repair_join(left,s[i]);
            if (!left.w || !suffix[i+1].w) continue;
            int gain=cursor_rect_area(*tight)-cursor_rect_area(left)-cursor_rect_area(suffix[i+1]);
            if (gain>best) { best=gain; pair[0]=left; pair[1]=suffix[i+1]; }
        }
    }
    return best;
}
static int cursor_repair_plan(const struct cursor_repair *p,cursor_rect out[CURSOR_REPAIR_OUTPUT_LIMIT])
{
    int n=0;
    for (int i=0;i<p->count;++i) {
        cursor_rect t,pair[2];
        (void)cursor_repair_split(p,p->rects[i],&t,pair);
        if (t.w) out[n++]=t;
    }
    while (n<CURSOR_REPAIR_OUTPUT_LIMIT) {
        int at=-1,best=0; cursor_rect winner[2];
        for (int i=0;i<n;++i) {
            cursor_rect t,pair[2];
            int gain=cursor_repair_split(p,out[i],&t,pair);
            if (gain>best) { best=gain; at=i; winner[0]=pair[0]; winner[1]=pair[1]; }
        }
        if (at<0) break;
        out[at]=winner[0]; out[n++]=winner[1];
    }
    return n;
}
static void cursor_repair_reset(struct cursor_repair *p)
{
    p->count=0;
    memset(p->tiles,0,sizeof(p->tiles));
    memset(p->pixels,0,sizeof(p->pixels));
}
static int cursor_repair_due(const struct cursor_repair *p,uint64_t now,int input_ready,int moved)
{
    return p->count && !input_ready && !moved && now>=p->last_activity &&
        now-p->last_activity>=CURSOR_REPAIR_IDLE_MS;
}
#endif
