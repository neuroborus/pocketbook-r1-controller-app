#define main overlay_program_main
#include "../src/r1-controller.c"
#undef main
#include <assert.h>

int main(void)
{
    struct r1_event_queue q = {0};
    struct input_event ev = {.type = EV_REL, .code = REL_X, .value = 28}, out;
    assert(!r1_queue_pop(&q, &out));
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < R1_QUEUE_CAPACITY; ++i) {
            ev.value = i + 1;
            assert(r1_queue_push(&q, ev) == 0);
        }
        assert(q.motion);
        assert(r1_queue_push(&q, ev) == -1);
        for (int i = 0; i < R1_QUEUE_CAPACITY; ++i) {
            assert(r1_queue_pop(&q, &out));
            assert(out.value == i + 1);
        }
        assert(!q.motion && !q.count);
        assert(r1_queue_push(&q, ev) == 0);
        assert(r1_queue_pop(&q, &out)); /* shift head to exercise wrapping */
    }
    assert(!click_frame_ready(599, 500, 0, 1));
    assert(!click_frame_ready(600, 299, 0, 1));
    assert(click_frame_ready(600, 300, 0, 1));
    assert(!click_frame_ready(2999, 1000, 0, 0));
    assert(!click_frame_ready(3000, 1000, 0, 0));
    assert(!click_frame_ready(60000, 59000, 1, 0));
    assert(!click_frame_ready(1199, 800, 1, 1));
    assert(!click_frame_ready(1200, 599, 1, 1));
    assert(click_frame_ready(1200, 600, 1, 1));
    ev = (struct input_event){.type = EV_KEY, .code = KEY_VOLUMEUP, .value = 1};
    assert(r1_queue_push(&q, ev) == 0 && q.click);
    assert(r1_queue_pop(&q, &out) && !q.click);
    /* Consumed button must not leave stale intent with a trailing SYN. */
    ev = (struct input_event){.type = EV_KEY, .code = BTN_LEFT, .value = 1};
    assert(r1_queue_push(&q, ev) == 0);
    ev = (struct input_event){.type = EV_SYN, .code = SYN_REPORT};
    assert(r1_queue_push(&q, ev) == 0);
    assert(r1_queue_pop(&q, &out) && !q.click && q.count == 1);
    assert(r1_queue_pop(&q, &out));

    /* Motion/SYN survives; a second circle during handoff must not be
     * replayed at the same coordinates in a different application. */
    int fds[2];
    assert(pipe(fds) == 0);
    r1 = fds[0];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    struct input_event input[] = {
        {.type = EV_REL, .code = REL_X, .value = 28},
        {.type = EV_SYN, .code = SYN_REPORT},
        {.type = EV_KEY, .code = BTN_LEFT, .value = 1},
        {.type = EV_KEY, .code = BTN_LEFT, .value = 0},
    };
    assert(write(fds[1], input, sizeof(input)) == sizeof(input));
    drain_while_clicking();
    assert(running && pending.count == 2 && pending.motion && !pending.click);
    for (size_t i = 0; i < 2; ++i) {
        assert(next_input(&out));
        assert(out.type == input[i].type && out.code == input[i].code && out.value == input[i].value);
    }
    assert(!next_input(&out));
    ev = (struct input_event){.type = EV_KEY, .code = BTN_RIGHT, .value = 1};
    ev.input_event_sec = 1;
    assert(write(fds[1], &ev, sizeof(ev)) == sizeof(ev));
    ev.value = 0; ev.input_event_sec = 2;
    assert(write(fds[1], &ev, sizeof(ev)) == sizeof(ev));
    drain_while_clicking();
    assert(!running);
    struct return_button b = {0};
    assert(return_button_event(&b, 0, 1) == 0);
    assert(return_button_event(&b, 1, 100) == 0);
    assert(return_button_event(&b, 2, 120) == 0);
    assert(return_button_event(&b, 0, 170) == 1);
    assert(return_button_event(&b, 0, 200) == 0);
    assert(return_button_event(&b, 1, 1000) == 0);
    assert(return_button_event(&b, 0, 2000) == 2);
    running = 1;
    ev = (struct input_event){.type = EV_SYN, .code = SYN_DROPPED};
    assert(write(fds[1], &ev, sizeof(ev)) == sizeof(ev));
    drain_while_clicking();
    assert(!running);
    running = 1;
    ev = (struct input_event){.type = EV_REL, .code = REL_Y, .value = 28};
    for (int i = 0; i < R1_QUEUE_CAPACITY; ++i)
        assert(r1_queue_push(&pending, ev) == 0);
    assert(write(fds[1], &ev, sizeof(ev)) == sizeof(ev));
    drain_while_clicking();
    assert(!running);
    close(fds[0]); close(fds[1]); r1 = -1;
    puts("PASS: queue ordering, wrap, overflow fail-safe, click-time input preservation, Return, SYN_DROPPED, resume timing");
    return 0;
}
