#ifndef RETURN_BUTTON_H
#define RETURN_BUTTON_H
#include <stdint.h>
struct return_button { uint64_t pressed; int down; };
/* 0=no action, 1=short Back, 2=long release stops the controller. */
static int return_button_event(struct return_button *b, int value, uint64_t ms)
{
    if (value == 1 && !b->down) { b->down = 1; b->pressed = ms; }
    if (value != 0 || !b->down) return 0;
    b->down = 0;
    return ms >= b->pressed && ms - b->pressed >= 1000 ? 2 : 1;
}
#endif
