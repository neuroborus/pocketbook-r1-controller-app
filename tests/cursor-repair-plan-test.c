#define main overlay_program_main
#include "../src/r1-controller.c"
#undef main
#include <assert.h>

static uint8_t sample[1264*1680*3];
static unsigned long total_old,total_new,total_calls,batches;
static void check_plan(struct cursor_repair *p)
{
    cursor_rect out[CURSOR_REPAIR_OUTPUT_LIMIT];
    int n=cursor_repair_plan(p,out),old=0,area=0;
    assert(n>0 && n<=CURSOR_REPAIR_OUTPUT_LIMIT);
    for (int i=0;i<p->count;++i) old+=cursor_rect_area(p->rects[i]);
    for (int i=0;i<n;++i) {
        cursor_rect r=out[i];
        assert(r.x>=0 && r.y>=0 && r.w>0 && r.h>0 && r.x+r.w<=1264 && r.y+r.h<=1680);
        assert(!((r.x|r.y|r.w|r.h)&3));
        area+=cursor_rect_area(r);
        for (int j=i+1;j<n;++j) assert(!cursor_rect_overlap(r,out[j]));
    }
    assert(area<=old);
    for (int y=0;y<REPAIR_ROWS;++y)
        for (int x=0;x<REPAIR_COLS;++x) {
            if (!cursor_repair_marked(p,x,y)) continue;
            int covered=0;
            for (int i=0;i<n;++i)
                covered|=x*4>=out[i].x && y*4>=out[i].y &&
                    x*4<out[i].x+out[i].w && y*4<out[i].y+out[i].h;
            assert(covered);
        }
    total_old+=(unsigned)old; total_new+=(unsigned)area;
    total_calls+=(unsigned)n; ++batches;
    cursor_repair_reset(p);
}
static void move_sample(struct cursor_repair *p,int ox,int oy,int x,int y)
{
    cursor_rect r[CURSOR_UPDATE_LIMIT];
    int n=cursor_update_plan(ox,oy,x,y,1264,1680,r);
    assert(n>=0);
    for (int i=0;i<n;++i) (void)cursor_repair_exposed(p,sample,1264*3,ox,oy,x,y,r[i]);
}
int main(int argc,char **argv)
{
    memset(sample,127,sizeof(sample)); cursor_init_mask();
    struct cursor_repair p={0};
    uint64_t begin=now_ms();
    if (argc==2) {
        FILE *f=fopen(argv[1],"r"); assert(f);
        char line[1024]; int ox=0,oy=0,have=0,moves=0;
        while (fgets(line,sizeof(line),f)) {
            int x,y;
            if (sscanf(line,"FRAME x=%d y=%d",&x,&y)==2) {
                if (have && strstr(line,"fast=1")) { move_sample(&p,ox,oy,x,y); ++moves; }
                ox=x; oy=y; have=1;
            }
            if (!strncmp(line,"GRAY_REPAIR ",12) && p.count) check_plan(&p);
        }
        if (p.count) check_plan(&p);
        fclose(f);
        printf("REPLAY moves=%d uniform-gray synthetic background, recorded positions/repair boundaries\n",moves);
    } else {
        for (int trial=0;trial<80;++trial) {
            int ox=(trial*71)%1240,oy=(trial*107)%1648;
            for (int step=0;step<30;++step) {
                int x=(ox+6+trial)%1240,y=(oy+3+trial*2)%1648;
                move_sample(&p,ox,oy,x,y); ox=x; oy=y;
            }
            assert(p.count); check_plan(&p);
        }
        cursor_repair_add(&p,(cursor_rect){0,0,1264,1680}); check_plan(&p);
        cursor_repair_add(&p,(cursor_rect){1260,1676,4,4}); check_plan(&p);
    }
    printf("PASS batches=%lu legacy_area=%lu new_area=%lu reduction=%.1f%% requests=%lu elapsed_ms=%llu (includes assertions)\n",
        batches,total_old,total_new,100.0*(total_old-total_new)/total_old,total_calls,
        (unsigned long long)(now_ms()-begin));
    return 0;
}
