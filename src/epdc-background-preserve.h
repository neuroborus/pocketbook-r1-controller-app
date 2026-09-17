#ifndef EPDC_BACKGROUND_PRESERVE_H
#define EPDC_BACKGROUND_PRESERVE_H
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "epdc-convert-abi.h"

static r1_epdc_instance *r1_epdc_backend;
static void (*r1_epdc_original_convert)(r1_epdc_img *,int,int,int,int,r1_epdc_img *,int,int,uint32_t);
static _Thread_local int r1_convert_scope;
static unsigned long r1_convert_preserved;

static void r1_preserving_convert(r1_epdc_img *src,int sx,int sy,int w,int h,
                                  r1_epdc_img *dst,int dx,int dy,uint32_t flags)
{
    /* Native hw_update converts synchronously before queueing the update.
     * Scope only our fast cursor requests, never native full refresh or other
     * app tasks. Keep waveform/inversion and all coordinates untouched. */
    if (r1_convert_scope && (flags & 255U) == 1 && (flags & R1_CONVERT_PURE) &&
        src && dst && src->depth == 24 && dst->depth == 8 && src->orientation == 0) {
        flags &= ~R1_CONVERT_PURE;
        ++r1_convert_preserved;
    }
    r1_epdc_original_convert(src,sx,sy,w,h,dst,dx,dy,flags);
}

/* Executable symbol interposition, enabled by --export-dynamic at link time.
 * Observe InkView's single initialization; replace one callback in its OWN
 * heap instance. No library files, shared memory or system settings patched. */
r1_epdc_instance *device_epdc_init(void)
{
    void *lib = dlopen("/usr/lib/libhwconfig.so",RTLD_NOW | RTLD_LOCAL);
    if (!lib) return NULL;
    void *sym = dlsym(lib,"device_epdc_init");
    r1_epdc_instance *(*original)(void);
    if (!sym) return NULL;
    memcpy(&original,&sym,sizeof(original));
    r1_epdc_backend = original();
    if (!r1_epdc_backend || r1_epdc_backend->version != 1 || r1_epdc_backend->type != 7 ||
        r1_epdc_backend->width != 1264 || r1_epdc_backend->height != 1680 ||
        !r1_epdc_backend->convert) return r1_epdc_backend;
    r1_epdc_original_convert = r1_epdc_backend->convert;
    r1_epdc_backend->convert = r1_preserving_convert;
    return r1_epdc_backend;
}

static int r1_epdc_preserve_ready(void)
{
    return r1_epdc_backend && r1_epdc_original_convert &&
        r1_epdc_backend->convert == r1_preserving_convert;
}
#endif
