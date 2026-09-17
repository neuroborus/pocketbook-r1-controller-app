#include <assert.h>
#include <stdio.h>
#include "../src/cursor-restore-mode.h"
#include "../src/cursor-repair.h"

#define W 128
#define H 192
static uint8_t saved[W*H*3], panel[W*H*3], expected[W*H*3];
static void apply_model(cursor_rect r,int gray)
{
    for (int y=r.y;y<r.y+r.h;++y)
        for (int x=r.x;x<r.x+r.w;++x) {
            size_t off=((size_t)y*W+(size_t)x)*3;
            const uint8_t *p=expected+off;
            int bw=(p[0]==0 && p[1]==0 && p[2]==0) || (p[0]==255 && p[1]==255 && p[2]==255);
            /* Deliberately model DU's inability to restore intermediate tones.
             * Not an emulator of the actual color panel or its timings. */
            if (gray || bw) memcpy(panel+off,p,3);
        }
}
static int exercise(int ox,int oy,int x,int y,int mixed)
{
    cursor_restore(panel,saved,W*3,(cursor_rect){0,0,W,H});
    cursor_draw(panel,W*3,ox,oy);
    memcpy(expected,saved,sizeof(saved)); cursor_draw(expected,W*3,x,y);
    cursor_rect r[CURSOR_UPDATE_LIMIT];
    int n=cursor_update_plan(ox,oy,x,y,W,H,r), restores=0;
    assert(n>=0 && n<=CURSOR_UPDATE_LIMIT);
    for(int i=0;i<n;++i) {
        int gray=mixed && cursor_restore_needs_gray(saved,W*3,ox,oy,x,y,r[i]);
        restores+=gray; apply_model(r[i],gray);
    }
    if (mixed) assert(memcmp(panel,expected,sizeof(panel))==0);
    else assert(memcmp(panel,expected,sizeof(panel))!=0);
    return restores;
}
static void deferred_sequence(void)
{
    struct cursor_repair p={0};
    int ox=30,oy=40;
    memcpy(panel,saved,sizeof(panel)); cursor_draw(panel,W*3,ox,oy);
    for (int step=0;step<1000;++step) {
        int x=20+(step*7)%70,y=30+(step*11)%120;
        memcpy(expected,saved,sizeof(expected)); cursor_draw(expected,W*3,x,y);
        cursor_rect r[CURSOR_UPDATE_LIMIT];
        int n=cursor_update_plan(ox,oy,x,y,W,H,r);
        assert(n>=0);
        for (int i=0;i<n;++i) {
            int need=cursor_restore_needs_gray(saved,W*3,ox,oy,x,y,r[i]);
            assert(cursor_repair_exposed(&p,saved,W*3,ox,oy,x,y,r[i])==need);
            apply_model(r[i],0);
        }
        assert(p.count>0 && p.count<=CURSOR_REPAIR_LIMIT);
        for (int i=0;i<p.count;++i) {
            assert(p.rects[i].x>=0 && p.rects[i].y>=0 && p.rects[i].x+p.rects[i].w<=W && p.rects[i].y+p.rects[i].h<=H);
            for (int j=i+1;j<p.count;++j) assert(!cursor_rect_overlap(p.rects[i],p.rects[j]));
        }
        ox=x; oy=y;
    }
    assert(memcmp(panel,expected,sizeof(panel))!=0); /* explicit temporary trails */
    p.last_activity=1000;
    assert(!cursor_repair_due(&p,1249,0,0));
    assert(!cursor_repair_due(&p,1250,1,0));
    assert(!cursor_repair_due(&p,1250,0,1));
    assert(!cursor_repair_due(&p,999,0,0));
    assert(cursor_repair_due(&p,1250,0,0));
    cursor_rect final[CURSOR_REPAIR_OUTPUT_LIMIT];
    int count=cursor_repair_plan(&p,final),area=0,legacy=0;
    assert(count>0 && count<=CURSOR_REPAIR_OUTPUT_LIMIT);
    for (int i=0;i<p.count;++i) legacy+=cursor_rect_area(p.rects[i]);
    for (int i=0;i<count;++i) {
        area+=cursor_rect_area(final[i]);
        for (int j=i+1;j<count;++j) assert(!cursor_rect_overlap(final[i],final[j]));
        apply_model(final[i],1);
    }
    assert(area<=legacy);
    assert(memcmp(panel,expected,sizeof(panel))==0);
    cursor_repair_reset(&p); assert(!cursor_repair_due(&p,2000,0,0));
    assert(cursor_repair_plan(&p,final)==0);
    cursor_repair_add(&p,(cursor_rect){4,8,24,32});
    assert(cursor_repair_plan(&p,final)==1 && cursor_rect_area(final[0])==24*32);
    cursor_repair_reset(&p);
    puts("PASS: 1000 fast moves, bounded disjoint repair debt, complete modeled idle repair, no repair before idle or with input/unrendered motion");
}
int main(void)
{
    cursor_init_mask();
    for (size_t i=0;i<sizeof(saved);++i) saved[i]=(uint8_t)(1+(i*37)%253);
    deferred_sequence();
    assert(cursor_needs_gray(saved,W*3,(cursor_rect){0,0,24,32}));
    exercise(30,40,60,80,0); /* old all-DU approach leaves a modeled ghost */
    int cases=0;
    for(int dx=-30;dx<=30;dx+=3)
        for(int dy=-30;dy<=30;dy+=3) {
            int restores=exercise(50,70,50+dx,70+dy,1);
            if (dx || dy) assert(restores>0);
            ++cases;
        }
    memset(saved,0,sizeof(saved));
    assert(exercise(30,40,60,80,1)==0);
    memset(saved,255,sizeof(saved));
    assert(exercise(30,40,60,80,1)==0);
    memset(saved,1,sizeof(saved)); /* even near-black may need a gray transition */
    assert(exercise(30,40,60,80,1)>0);
    printf("PASS: %d colored-background moves restore all modeled pixels; pure BW avoids gray; stationary and near-black covered\n",cases);
}
