#define main overlay_program_main
#include "../src/r1-controller.c"
#undef main
#include <assert.h>
#define W 128
#define H 192
static uint8_t target[W*H*3],surface[W*H*3],cached[W*H*3],optical[W*H*3],bg[W*H*3];
static icanvas test_canvas={.width=W,.height=H,.scanline=W*3,.depth=24,.addr=surface};
static int pending_updates,du_calls,gray_calls;
static icanvas *test_get_canvas(void) { return &test_canvas; }
static int test_pending(void) { if (pending_updates) { --pending_updates; return 1; } return 0; }
static void test_wait(void) { assert(!pending_updates); }
static void test_update(int x,int y,int w,int h,int flags)
{
    assert(!(flags & (UPD_CLEAR | (1<<16))));
    int mode=flags&255; assert(mode==1 || mode==2);
    assert(r1_convert_scope==(mode==1));
    if (mode==1) ++du_calls; else ++gray_calls;
    for (int py=y;py<y+h;++py)
        for (int px=x;px<x+w;++px) {
            size_t off=((size_t)py*W+(size_t)px)*3;
            const uint8_t *v=surface+off;
            int binary=(v[0]==0 && v[1]==0 && v[2]==0) || (v[0]==255 && v[1]==255 && v[2]==255);
            /* HYPOTHETICAL controller model, NOT actual firmware emulation:
             * DU accepts a target in its cache but drives only binary pixels;
             * partial gray does not drive a cache-identical pixel. */
            if ((mode==1 && binary) || (mode==2 && memcmp(cached+off,v,3)))
                memcpy(optical+off,v,3);
            memcpy(cached+off,v,3);
            if (memcmp(v,target+off,3)) {
                assert(mode==1 && cursor_repair_pixel(&repair,px,py));
                assert(binary); /* only exact debt pixels can be primed */
            }
        }
    pending_updates=2;
}
static void scenario(int shade)
{
    cursor_repair_reset(&repair);
    memset(bg,shade,sizeof(bg)); memcpy(surface,bg,sizeof(surface));
    cursor_draw(surface,W*3,30,40);
    memcpy(cached,surface,sizeof(cached)); memcpy(optical,surface,sizeof(optical));
    int ox=30,oy=40;
    for (int step=0;step<30;++step) {
        int x=20+(step*7)%70,y=30+(step*11)%120;
        memcpy(target,bg,sizeof(target)); cursor_draw(target,W*3,x,y);
        memcpy(surface,target,sizeof(surface));
        cursor_rect r[CURSOR_UPDATE_LIMIT];
        int n=cursor_update_plan(ox,oy,x,y,W,H,r); assert(n>=0);
        for (int i=0;i<n;++i) {
            (void)cursor_repair_exposed(&repair,bg,W*3,ox,oy,x,y,r[i]);
            update_rect(r[i],1);
        }
        assert(wait_display()==0); ox=x; oy=y;
    }
    assert(repair.count && memcmp(optical,target,sizeof(target)));
    /* Reproduce partial-only cleanup failure before testing pixel priming. */
    for (int i=0;i<repair.count;++i) update_rect(repair.rects[i],3);
    assert(wait_display()==0 && memcmp(optical,target,sizeof(target)));
    int before_du=du_calls,before_gray=gray_calls;
    assert(flush_cursor_repair()==0 && !repair.count && !pending_updates);
    assert(du_calls>before_du && gray_calls>before_gray);
    assert(!memcmp(surface,target,sizeof(target)) && !memcmp(optical,target,sizeof(target)));
    for (size_t i=0;i<sizeof(repair.pixels);++i) assert(!repair.pixels[i]);
    /* Hide the current arrow: preserve opaque silhouette, no rectangular seed. */
    memcpy(target,bg,sizeof(target)); memcpy(surface,target,sizeof(surface));
    cursor_repair_hide(&repair,ox,oy,cursor_dirty(ox,oy,ox,oy,W,H));
    assert(flush_cursor_repair()==0);
    assert(!memcmp(surface,target,sizeof(target)) && !memcmp(optical,target,sizeof(target)));
}
int main(void)
{
    canvas=&test_canvas; canvas_snapshot=test_canvas; canvas_size=sizeof(surface);
    iv.canvas=test_get_canvas; iv.update=test_update; iv.update_pending=test_pending; iv.wait=test_wait;
    cursor_init_mask(); running=1;
    scenario(40); scenario(127); scenario(200);
    puts("PASS: actual repair pipeline primes exact pixels only, preserves current cursor/target, waits both phases, resets debt, hides cursor; partial-only fails in the stated model");
    return 0;
}
