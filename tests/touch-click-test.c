/* Exercises mapping and the actual emitter through a pipe, never an input device. */
#define main overlay_program_main
#include "../src/r1-controller.c"
#undef main
#include <assert.h>

int main(void)
{
    int tx, ty;
    assert(touch_map(0, 0, 1264, 1680, 0, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 0 && ty == 0);
    assert(touch_map(1263, 1679, 1264, 1680, 0, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 1448 && ty == 1072);
    assert(touch_map(555, 826, 1264, 1680, 0, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 636 && ty == 527); /* Matches the previously successful device log. */
    assert(touch_map(0, 0, 1264, 1680, 3, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 1448 && ty == 1072);
    assert(touch_map(0, 0, 1680, 1264, 1, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 0 && ty == 1072);
    assert(touch_map(0, 0, 1680, 1264, 2, 0, 1448, 0, 1072, &tx, &ty) == 0);
    assert(tx == 1448 && ty == 0);
    assert(touch_map(-3, 2000, 1264, 1680, 0, 10, 1458, 20, 1092, &tx, &ty) == 0);
    assert(tx == 10 && ty == 1092);
    assert(touch_map(1, 1, 1, 1, 0, 0, 1448, 0, 1072, &tx, &ty) == -1);
    assert(touch_map(1, 1, 1264, 1680, 4, 0, 1448, 0, 1072, &tx, &ty) == -1);
    int fds[2];
    assert(pipe(fds) == 0);
    assert(emit_tap(fds[1], 636, 527, 20001) == 0);
    close(fds[1]);
    struct input_event events[12];
    ssize_t size = read(fds[0], events, sizeof(events));
    assert(size == (ssize_t)(11 * sizeof(events[0])));
    close(fds[0]);
    assert(events[0].type == EV_ABS && events[0].code == ABS_MT_SLOT && events[0].value == 0);
    assert(events[1].code == ABS_MT_TRACKING_ID && events[1].value == 20001);
    assert(events[3].code == ABS_MT_POSITION_X && events[3].value == 636);
    assert(events[4].code == ABS_MT_POSITION_Y && events[4].value == 527);
    assert(events[5].type == EV_KEY && events[5].code == BTN_TOUCH && events[5].value == 1);
    assert(events[6].type == EV_SYN && events[6].code == SYN_REPORT);
    assert(events[7].code == ABS_MT_SLOT && events[7].value == 0);
    assert(events[8].code == ABS_MT_TRACKING_ID && events[8].value == -1);
    assert(events[9].type == EV_KEY && events[9].code == BTN_TOUCH && events[9].value == 0);
    assert(events[10].type == EV_SYN && events[10].code == SYN_REPORT);
    assert(pipe(fds) == 0);
    assert(emit_short_key(fds[1], KEY_POWER) == 0);
    close(fds[1]);
    size = read(fds[0], events, sizeof(events));
    assert(size == (ssize_t)(4 * sizeof(events[0])));
    close(fds[0]);
    assert(events[0].type == EV_KEY && events[0].code == KEY_POWER && events[0].value == 1);
    assert(events[1].type == EV_SYN && events[1].code == SYN_REPORT);
    assert(events[2].type == EV_KEY && events[2].code == KEY_POWER && events[2].value == 0);
    assert(events[3].type == EV_SYN && events[3].code == SYN_REPORT);
    puts("PASS: touch mapping, paired tap and short hardware key down/up with SYN reports");
    return 0;
}
