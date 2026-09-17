#ifndef CONTROLLER_MAP_H
#define CONTROLLER_MAP_H

#include <linux/input.h>
#include <stdint.h>
#include <string.h>

#define CONTROLLER_NAV 1024
enum controller_mode { CONTROLLER_MOUSE, CONTROLLER_GAMEPAD };

struct controller_map {
    enum controller_mode mode;
    unsigned char down[KEY_MAX + 1];
    int axes[2];
    uint64_t guard;
};

static void controller_map_reset(struct controller_map *m)
{
    memset(m, 0, sizeof(*m));
    m->axes[0] = m->axes[1] = 1;
}

/* Pointer/Return/mouse-C are handled by the main input loop. Extra controls
 * return one action on a paired release; companion keys cannot double-fire.
 * Raw R1 button codes are mode-dependent; see docs/KEY_MAP.md. */
static int controller_map_event(struct controller_map *m, const struct input_event *e, uint64_t now)
{
    enum controller_mode mode = m->mode;
    if (e->type == EV_REL && (e->code == REL_X || e->code == REL_Y)) mode = CONTROLLER_MOUSE;
    if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) mode = CONTROLLER_GAMEPAD;
    if (e->type == EV_KEY) {
        if (e->code == 308 || e->code == 307 || e->code == 310 || e->code == 311)
            mode = CONTROLLER_GAMEPAD;
        if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == 115 || e->code == 114)
            mode = CONTROLLER_MOUSE;
    }
    if (mode != m->mode) {
        controller_map_reset(m);
        m->mode = mode;
        m->guard = now + 350;
    }
    if (e->type == EV_ABS && (e->code == ABS_X || e->code == ABS_Y)) {
        if (e->value < 0 || e->value > 2) return 0;
        int axis = e->code == ABS_Y, old = m->axes[axis];
        m->axes[axis] = e->value;
        if (old == e->value || e->value == 1) return 0;
        return CONTROLLER_NAV + (axis ? (e->value == 0 ? 0x11 : 0x12) : (e->value == 0 ? 0x13 : 0x14));
    }
    if (e->type != EV_KEY || e->code > KEY_MAX) return 0;
    int action = 0;
    if (m->mode == CONTROLLER_MOUSE) {
        if (e->code == 304) action = KEY_PAGEUP;   /* A */
        if (e->code == 305) action = KEY_PAGEDOWN; /* B */
        if (e->code == 114) action = KEY_BACK;     /* D */
    } else {
        if (e->code == 308) action = KEY_PAGEUP;   /* A */
        if (e->code == 304) action = KEY_PAGEDOWN; /* B */
        if (e->code == 307) action = KEY_POWER;    /* C */
        if (e->code == 305) action = KEY_BACK;     /* D */
        if (e->code == 310) action = CONTROLLER_NAV + 0x0a; /* Circle: InkView OK */
    }
    if (!action) return 0;
    if (e->value == 1) {
        m->down[e->code] = now >= m->guard;
        return 0;
    }
    if (e->value != 0) return 0;
    int held = m->down[e->code];
    m->down[e->code] = 0;
    return held && now >= m->guard ? action : 0;
}
#endif
