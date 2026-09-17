#ifndef EPDC_CONVERT_ABI_H
#define EPDC_CONVERT_ABI_H
#include <stdint.h>
#include <stddef.h>
/* Exact public ABI from SDK610 hwconfig.h:315-369. Native libhwconfig
 * allocates80bytes for instance; native InkView calls convert at offset72. */
typedef struct {
    int width, height, scanline, depth, orientation;
    void *data;
} r1_epdc_img;
typedef struct {
    int version, type, alignment, fd;
    uint8_t *framebuffer;
    int width, height, scanline, depth;
    uint32_t memsize;
    void (*rotate)(int);
    int (*update)(int,int,int,int,int,int);
    int (*busy)(void);
    void (*getwftimes)(uint16_t[16]);
    void (*sync)(void);
    void (*lock)(void);
    void (*unlock)(void);
    int (*copy)(r1_epdc_img *,int,int,int,int);
    void (*convert)(r1_epdc_img *,int,int,int,int,r1_epdc_img *,int,int,uint32_t);
    int (*suspend)(void);
} r1_epdc_instance;
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(r1_epdc_img) == 24, "epdc_img ABI");
_Static_assert(sizeof(r1_epdc_instance) == 80, "epdc_instance ABI");
_Static_assert(offsetof(r1_epdc_instance,convert) == 72, "convert ABI");
#endif
#define R1_CONVERT_PURE (1U << 9)
#define R1_CONVERT_INVERSION (1U << 10)
#endif
