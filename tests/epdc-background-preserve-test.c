#include <assert.h>
#include <pthread.h>
#include "../src/epdc-background-preserve.h"

static uint32_t received;
static int calls;
static r1_epdc_img input = {64,64,192,24,0,NULL}, output = {64,64,64,8,0,NULL};
static void fake_convert(r1_epdc_img *src,int sx,int sy,int w,int h,
                         r1_epdc_img *dst,int dx,int dy,uint32_t flags)
{
    assert(src==&input && dst==&output && sx==3 && sy==5 && w==8 && h==12 && dx==7 && dy==9);
    received = flags; ++calls;
}
static void invoke(uint32_t flags, uint32_t expected)
{
    int before = calls;
    r1_preserving_convert(&input,3,5,8,12,&output,7,9,flags);
    assert(calls==before+1 && received==expected);
}
static void *other_thread(void *arg)
{
    (void)arg;
    assert(r1_convert_scope==0);
    invoke(0x601,0x601);
    return NULL;
}
int main(void)
{
    assert(!r1_epdc_preserve_ready());
    r1_epdc_instance backend = {.convert=r1_preserving_convert};
    r1_epdc_backend=&backend; r1_epdc_original_convert=fake_convert;
    assert(r1_epdc_preserve_ready());
    invoke(0x601,0x601); /* Outside our cursor call: unchanged */
    r1_convert_scope=1;
    invoke(0x601,0x401); /* DU + PURE + inversion -> DU + inversion */
    invoke(0xa01,0x801); /* Preserve full bit too */
    invoke(0x401,0x401); /* No PURE: unchanged */
    invoke(0x602,0x602); /* Grayscale: unchanged */
    invoke(0x608,0x608); /* DU4: unchanged */
    input.depth=8; invoke(0x601,0x601); input.depth=24;
    output.depth=24; invoke(0x601,0x601); output.depth=8;
    input.orientation=1; invoke(0x601,0x601); input.orientation=0;
    pthread_t thread;
    assert(pthread_create(&thread,NULL,other_thread,NULL)==0);
    assert(pthread_join(thread,NULL)==0);
    assert(r1_convert_scope==1 && r1_convert_preserved==2);
    r1_convert_scope=0;
    invoke(0x601,0x601);
    puts("PASS: only PURE cleared inside cursor DU scope; waveform/inversion/coords retained; other modes/depths/rotation/thread unchanged");
}
