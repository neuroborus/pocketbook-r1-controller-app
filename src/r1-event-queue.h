#ifndef R1_EVENT_QUEUE_H
#define R1_EVENT_QUEUE_H
#include <linux/input.h>
#include <stddef.h>

/* Preserve event ordering (including SYN and button pairs) during focus handoff.
 * A full queue is an error, never silent loss of a release event. */
#define R1_QUEUE_CAPACITY 1024
struct r1_event_queue {
    struct input_event events[R1_QUEUE_CAPACITY];
    size_t head, count;
    int motion, click;
};

static int r1_queue_push(struct r1_event_queue *q, struct input_event ev)
{
    if (q->count == R1_QUEUE_CAPACITY) return -1;
    q->events[(q->head + q->count++) % R1_QUEUE_CAPACITY] = ev;
    if (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y) && ev.value)
        ++q->motion;
    if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == KEY_VOLUMEUP) && ev.value == 1)
        ++q->click;
    return 0;
}

static int r1_queue_pop(struct r1_event_queue *q, struct input_event *ev)
{
    if (!q->count) return 0;
    *ev = q->events[q->head];
    if (ev->type == EV_REL && (ev->code == REL_X || ev->code == REL_Y) && ev->value)
        --q->motion;
    if (ev->type == EV_KEY && (ev->code == BTN_LEFT || ev->code == KEY_VOLUMEUP) && ev->value == 1)
        --q->click;
    q->head = (q->head + 1) % R1_QUEUE_CAPACITY;
    if (!--q->count) q->motion = q->click = 0;
    return 1;
}

static int click_frame_ready(unsigned long long elapsed,
    unsigned long long stable, int changed, int motion)
{
    if (!motion) return 0; /* Keep the real app foreground until user resumes. */
    if (changed) return elapsed >= 1200 && stable >= 600;
    return elapsed >= 600 && stable >= 300;
}
#endif
