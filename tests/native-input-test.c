#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
static mqd_t fake_open(const char *name, int flags, ...);
static int fake_getattr(mqd_t queue, struct mq_attr *attr);
static int fake_close(mqd_t queue);
static int fake_ioctl(int fd, unsigned long request, ...);
#define mq_open fake_open
#define mq_getattr fake_getattr
#define mq_close fake_close
#define ioctl fake_ioctl
#define main controller_program_main
#include "../src/r1-controller.c"
#undef main
#undef ioctl
#undef mq_close
#undef mq_getattr
#undef mq_open

static int fail_open, fail_state, fail_touch, finger, closes, touch_queries;
static long message_size = 40, queued;
static mqd_t fake_open(const char *name, int flags, ...)
{
    assert(!strcmp(name, "/hwevent"));
    assert(flags == (O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fail_open) { errno = ENOENT; return (mqd_t)-1; }
    return 17;
}
static int fake_getattr(mqd_t queue, struct mq_attr *attr)
{
    assert(queue == 17);
    if (fail_state) { errno = EIO; return -1; }
    *attr = (struct mq_attr){.mq_msgsize = message_size, .mq_curmsgs = queued};
    return 0;
}
static int fake_close(mqd_t queue) { assert(queue == 17); ++closes; return 0; }
static int fake_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42);
    const size_t words = (KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long));
    assert(request == EVIOCGKEY(words * sizeof(unsigned long)));
    ++touch_queries;
    if (fail_touch) { errno = EIO; return -1; }
    va_list args;
    va_start(args, request);
    unsigned long *keys = va_arg(args, unsigned long *);
    unsigned bits = 8U * sizeof(unsigned long);
    if (finger) keys[BTN_TOUCH / bits] |= 1UL << (BTN_TOUCH % bits);
    va_end(args);
    return 0;
}

int main(void)
{
    assert(native_input_busy() == 0); /* Diagnostics do not open the queue. */
    fail_open = 1;
    assert(init_hardware_queue() == -1 && hardware_queue == (mqd_t)-1);
    fail_open = 0; message_size = 64;
    assert(init_hardware_queue() == -1 && hardware_queue == (mqd_t)-1 && closes == 1);
    message_size = 40; fail_state = 1;
    assert(init_hardware_queue() == -1 && hardware_queue == (mqd_t)-1 && closes == 2);
    fail_state = 0;
    assert(init_hardware_queue() == 0);
    touch_fd = 42;
    assert(native_input_busy() == 0);
    queued = 3; int before = touch_queries;
    assert(native_input_busy() == 1 && queued == 3 && touch_queries == before);
    queued = 0; finger = 1;
    assert(native_input_busy() == 1); /* No steal between move/hold reports. */
    finger = 0;
    assert(native_input_busy() == 0);
    fail_touch = 1;
    assert(native_input_busy() == -1);
    fail_touch = 0; fail_state = 1;
    assert(native_input_busy() == -1);
    fail_state = 0; touch_fd = -1;
    assert(native_input_busy() == -1);
    cleanup();
    assert(hardware_queue == (mqd_t)-1 && closes == 3);
    cleanup();
    assert(closes == 3);
    puts("PASS: read-only queue observation, native ABI guard, held-finger priority, error handling and descriptor cleanup; no hardware or real queue");
}
