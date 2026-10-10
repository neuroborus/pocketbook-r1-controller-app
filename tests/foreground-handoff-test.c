#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <mqueue.h>
static int fake_clock(clockid_t id, struct timespec *ts);
static int fake_sleep(useconds_t us);
static int fake_ioctl(int fd, unsigned long request, ...);
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout);
static int fake_mq_getattr(mqd_t queue, struct mq_attr *attr);
static ssize_t fake_read(int fd, void *buffer, size_t size);
#define clock_gettime fake_clock
#define usleep fake_sleep
#define ioctl fake_ioctl
#define poll fake_poll
#define mq_getattr fake_mq_getattr
#define read fake_read
#define main overlay_program_main
#include "../src/r1-controller.c"
#undef main
#undef read
#undef mq_getattr
#undef ioctl
#undef poll
#undef usleep
#undef clock_gettime
#include <assert.h>
#include <stdarg.h>

static uint64_t simulated_ms;
static uint64_t stop_at;
static int frame_copies;
static uint64_t first_capture_ms, focus_ms, task_switch_ms = 1500;
static int changing_frames;
static int missing_task_info;
static int interrupt_poll;
static int active_id, input_writer, inject_motion, injected, captured_ready;
static int inject_back, back_injected, early_motion;
static int inject_buttons, buttons_injected;
static int sent_taps, update_queue, queue_checks, changed_task;
static int native_reader, transform_shift, physical_touch;
static int native_queued, native_delivered, second_motion;
static uint64_t native_hold_until;
enum { TOUCH_DURING_COPY = 1, TOUCH_BEFORE_RECLAIM, TOUCH_DURING_ACTIVATION };
static int touch_race, touch_raced, reclaim_checks, stale_reports;
static int bw_updates, async_gray_updates;
static unsigned char pixels[128 * 192 * 3];
static icanvas test_canvas = {.width = 128, .height = 192, .scanline = 384,
    .depth = 24, .addr = pixels};

static int fake_clock(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)(simulated_ms / 1000);
    ts->tv_nsec = (long)(simulated_ms % 1000) * 1000000;
    return 0;
}
static int fake_sleep(useconds_t us)
{
    /* Virtual time only: no wall-clock sleep, including the ten-minute case. */
    simulated_ms += us / 1000;
    if (stop_at && simulated_ms>=stop_at) running=0;
    if (touch_raced && simulated_ms >= 13500) {
        assert(active_id != self || second_motion);
        native_queued = physical_touch = 0;
    }
    if ((native_hold_until || touch_raced) && !second_motion && simulated_ms >= 18000) {
        second_motion = 1;
        injected = 0; /* A new movement after finger-up, not held-gesture replay. */
    }
    if (inject_buttons && !buttons_injected && simulated_ms >= 2000) {
        struct input_event ev[] = {
            {.type = EV_KEY, .code = BTN_LEFT, .value = 1},
            {.type = EV_KEY, .code = BTN_LEFT, .value = 0},
            {.type = EV_KEY, .code = KEY_VOLUMEUP, .value = 1},
            {.type = EV_KEY, .code = KEY_VOLUMEUP, .value = 0},
            {.type = EV_SYN, .code = SYN_REPORT}
        };
        assert(write(input_writer, ev, sizeof(ev)) == sizeof(ev));
        buttons_injected = 1;
    }
    if (inject_back && !back_injected && simulated_ms >= 2000) {
        struct input_event ev[2] = {
            {.type = EV_KEY, .code = BTN_RIGHT, .value = 1},
            {.type = EV_KEY, .code = BTN_RIGHT, .value = 0}
        };
        ev[0].input_event_sec = 2;
        ev[1].input_event_sec = 2; ev[1].input_event_usec = 70000;
        assert(write(input_writer, ev, sizeof(ev)) == sizeof(ev));
        back_injected = 1;
    }
    if (inject_motion && !injected && simulated_ms >= (early_motion ? 3000U : 12000U)) {
        struct input_event ev[] = {
            {.type = EV_REL, .code = REL_X, .value = 28},
            {.type = EV_SYN, .code = SYN_REPORT}
        };
        assert(write(input_writer, ev, sizeof(ev)) == sizeof(ev));
        injected = 1;
    }
    return 0;
}
static int fake_ioctl(int fd, unsigned long request, ...)
{
    (void)fd; (void)request;
    if (physical_touch || (native_hold_until && simulated_ms < native_hold_until)) {
        va_list ap;
        va_start(ap, request);
        unsigned long *keys = va_arg(ap, unsigned long *);
        unsigned bits = 8U * sizeof(unsigned long);
        keys[BTN_TOUCH / bits] |= 1UL << (BTN_TOUCH % bits);
        va_end(ap);
    }
    return 0; /* input key bitmap was initialized to zero by caller */
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (interrupt_poll) { interrupt_poll = 0; errno = EINTR; return -1; }
    return poll(fds, count, timeout);
}
static int fake_mq_getattr(mqd_t queue, struct mq_attr *attr)
{
    assert(queue == 17);
    if (reclaim_checks && !--reclaim_checks) {
        native_queued = physical_touch = touch_raced = 1;
    }
    *attr = (struct mq_attr){.mq_msgsize = 40, .mq_curmsgs = native_queued};
    return 0;
}
static ssize_t fake_read(int fd, void *buffer, size_t size)
{
    if (stale_reports && fd == r1) {
        assert(size == sizeof(struct input_event));
        struct input_event ev = {.type = EV_REL, .code = REL_X, .value = 28};
        memcpy(buffer, &ev, sizeof(ev));
        --stale_reports;
        return sizeof(ev);
    }
    return read(fd, buffer, size);
}
static void fake_active(int *task, int *sub)
{
    if (changed_task && active_id != self && simulated_ms >= task_switch_ms) active_id = 11;
    *task = active_id; *sub = 0;
}
static int fake_activate(int task, int sub)
{
    (void)sub;
    if (native_hold_until && task == previous) {
        assert(native_queued == 1 && !sent_taps);
        assert(!bw_updates && !async_gray_updates);
        native_queued = 0; /* Stock app, not mapper, receives the first event. */
        native_delivered = 1;
    }
    if (task == self) {
        if (touch_race == TOUCH_DURING_ACTIVATION && !touch_raced) {
            native_queued = physical_touch = touch_raced = 1;
        } else if (touch_raced) {
            assert(second_motion && simulated_ms >= 18000 && !physical_touch && !native_queued);
        }
        if (native_hold_until) assert(native_delivered && second_motion && simulated_ms >= 18000);
        assert(simulated_ms >= (early_motion ? 10000U : 12000U) && injected && captured_ready);
        focus_ms = simulated_ms;
    }
    active_id = task;
    return 1;
}
static taskinfo *fake_info(int task)
{
    if (missing_task_info && simulated_ms >= 12300 && simulated_ms < 18000) return NULL;
    static taskinfo info;
    info = (taskinfo){.task = task, .mainpid = getpid(), .appname =
        simulated_ms < 10000 ? "/ebrmain/bin/eink-cache-reader.app" : "simulated-reader"};
    if (native_reader) info.appname = "/ebrmain/bin/eink-reader.app";
    return &info;
}
static void fake_copy(void)
{
    if (!frame_copies) first_capture_ms = simulated_ms;
    ++frame_copies;
    assert(active_id != self);
    /* A completely stable loading image for ten seconds, then actual book. */
    memset(pixels, simulated_ms < 10000 ? 40 : 200, sizeof(pixels));
    if (changing_frames) pixels[0] = (unsigned char)frame_copies;
    if (simulated_ms >= 10000) captured_ready = 1;
    if (touch_race && !touch_raced && simulated_ms >= 13200) {
        if (touch_race == TOUCH_DURING_COPY)
            native_queued = physical_touch = touch_raced = 1;
        if (touch_race == TOUCH_BEFORE_RECLAIM) reclaim_checks = 2;
    }
}
static icanvas *fake_canvas(void) { return &test_canvas; }
static void fake_wait(void) {}
static void fake_update(int x, int y, int w, int h, int flags)
{
    if (touch_race) assert(second_motion && !physical_touch && !native_queued);
    (void)x; (void)y; (void)w; (void)h;
    int mode = flags & 255;
    assert(mode == 1 || mode == 2);
    assert(r1_convert_scope == (mode == 1));
    if (mode == 1) {
        assert(!(flags & UPD_CLEAR));
        ++bw_updates;
    }
    if (mode == 2 && (flags & UPD_NOREFRESH)) {
        assert(!(flags & UPD_WAIT_AFTER));
        assert(!(flags & UPD_CLEAR)); /* Repair uses endpoint + partial gray. */
        assert(!(flags & (1 << 16))); /* never expand repair to full screen */
        ++async_gray_updates;
    }
    update_queue = 2;
}
static int fake_pending(void)
{
    ++queue_checks;
    if (update_queue) { --update_queue; return 1; }
    return 0;
}
static int fake_send_event(int task, int type, int x, int y)
{
    assert(task == active_id && task != self && x == 32 && y == 42);
    assert(type == (sent_taps % 2 ? 29 : 30));
    ++sent_taps;
    return 1;
}
static int fake_zero(void) { return 0; }
static bool fake_false(void) { return false; }
static int fake_transform(void *event)
{ int *p = event; p[2] += transform_shift; return 1; }

static void native_tap_test(void)
{
    int output[2];
    assert(pipe(output) == 0);
    assert(fcntl(output[0], F_SETFL, O_NONBLOCK) == 0);
    canvas = &test_canvas; touch_fd = output[1]; native_reader = 1;
    for (int i = 0; i < 2; ++i) {
        assert(emit_app_tap(10, 32, 42) == 0);
        struct input_event ev[12];
        assert(read(output[0], ev, sizeof(ev)) == (ssize_t)(11 * sizeof(ev[0])));
        assert(ev[3].code == ABS_MT_POSITION_X && ev[3].value == 32);
        assert(ev[4].code == ABS_MT_POSITION_Y && ev[4].value == 42);
        assert(ev[5].code == BTN_TOUCH && ev[5].value == 1);
        assert(ev[8].code == ABS_MT_TRACKING_ID && ev[8].value == -1);
        assert(ev[9].code == BTN_TOUCH && ev[9].value == 0);
    }
    transform_shift = 1;
    assert(emit_app_tap(10, 32, 42) == 0);
    transform_shift = 0; physical_touch = 1;
    assert(emit_app_tap(10, 32, 42) == 0);
    physical_touch = 0;
    assert(emit_app_tap(10, 128, 42) == 0);
    struct input_event ev;
    assert(read(output[0], &ev, sizeof(ev)) < 0 && errno == EAGAIN);
    assert(sent_taps == 0);
    close(output[0]); close(output[1]); touch_fd = -1; native_reader = 0;
    puts("PASS: reader native MT exact pixels, paired release, no duplicate addressed events, transform/touch/bounds guards");
}

static void scenario(int top, int resume)
{
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    r1 = input[0]; input_writer = input[1];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    touch_fd = top_key_fd = gpio_fd = output[1];
    touch_x = (struct input_absinfo){.maximum = 1448};
    touch_y = (struct input_absinfo){.maximum = 1072};
    self = 20; previous = active_id = 10; previous_sub = 0;
    canvas = &test_canvas; canvas_snapshot = *canvas; canvas_size = sizeof(pixels);
    memset(pixels, 255, sizeof(pixels));
    background = malloc(canvas_size); assert(background);
    memcpy(background, pixels, canvas_size);
    memset(&pending, 0, sizeof(pending));
    running = 1; simulated_ms = 0; session_deadline = 20000;
    inject_motion = resume; injected = captured_ready = 0;
    back_injected = 0; return_button = (struct return_button){0};
    buttons_injected = 0; c_down = 0;
    sent_taps = update_queue = queue_checks = 0;
    repair=(struct cursor_repair){0};
    cursor_repair_add(&repair,(cursor_rect){80,100,24,32});
    int result = click_through(30, 40, top);
    assert(repair.count==0); /* handoff clears accumulated trails before the app */
    assert(result == (resume ? 0 : -1));
    assert(active_id == (resume ? self : previous));
    assert(captured_ready);
    if (resume) assert(pending.motion && background[0] == 200);
    else assert(simulated_ms >= session_deadline);
    assert(queue_checks > 0 && update_queue == 0);
    close(output[1]);
    struct input_event events[24];
    ssize_t n = read(output[0], events, sizeof(events));
    int event_count = (top ? 4 : 0) + (inject_back ? 4 : 0) + (inject_buttons ? 4 : 0);
    assert(n == (ssize_t)(event_count * sizeof(events[0])));
    if (top) assert(events[0].code == top && events[2].value == 0);
    if (inject_back) {
        int i = top ? 4 : 0;
        assert(events[i].code == KEY_BACK && events[i].value == 1);
        assert(events[i+2].code == KEY_BACK && events[i+2].value == 0);
    }
    if (inject_buttons) {
        int i = top ? 4 : 0;
        assert(events[i].code == KEY_POWER && events[i].value == 1);
        assert(events[i+2].code == KEY_POWER && events[i+2].value == 0);
        assert(!pending.click); /* no repeated tap, no cursor reclaim for C */
    }
    assert(sent_taps == (top ? 0 : inject_buttons && !changed_task ? 4 : 2));
    if (resume) {
        int before_bw = bw_updates;
        int before_gray = async_gray_updates;
        assert(render_cursor(30, 40, 30, 80, 1) == 0);
        assert(bw_updates > before_bw && bw_updates <= before_bw + CURSOR_UPDATE_LIMIT);
        assert(async_gray_updates == before_gray && repair.count>0);
        assert(!cursor_repair_due(&repair,simulated_ms+249,0,0));
        assert(cursor_repair_due(&repair,simulated_ms+250,0,0));
        assert(flush_cursor_repair()==0 && repair.count==0 && update_queue==0);
        assert(async_gray_updates>before_gray && async_gray_updates-before_gray<=CURSOR_REPAIR_OUTPUT_LIMIT);
        assert(wait_display() == 0 && update_queue == 0);
        assert(r1_convert_scope == 0);
    }
    close(output[0]); close(input[0]); close(input[1]);
    r1 = touch_fd = top_key_fd = gpio_fd = -1;
    free(background); background = NULL;
}

static void application_idle_test(void)
{
    int input[2];
    assert(pipe(input)==0);
    r1=input[0]; input_writer=input[1];
    assert(fcntl(r1,F_SETFL,O_NONBLOCK)==0);
    application_mode=1; running=1; simulated_ms=0; stop_at=610000;
    session_deadline=UINT64_MAX; frame_copies=0;
    inject_motion=inject_back=inject_buttons=changed_task=0;
    pending=(struct r1_event_queue){0};
    self=20; previous=active_id=10;
    assert(await_foreground(30,40,0)==-1);
    assert(simulated_ms>=610000 && frame_copies==0 && active_id==10);
    close(input[0]); close(input[1]); r1=-1;
    application_mode=0; stop_at=0;
    puts("PASS: application stays foreground over ten simulated minutes, no idle RGB copies, interruptible stop");
}

static void foreground_queue_test(void)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = input[0];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    pending = (struct r1_event_queue){0};
    controller_map_reset(&mapping);
    running = 1;
    /* Held buttons/repeats/neutral axes/SYN are not pointer movement. Enough
     * reports to overflow the old queue, without real waiting or hardware. */
    struct input_event report[] = {
        {.type = EV_KEY, .code = 304, .value = 1},
        {.type = EV_KEY, .code = 304, .value = 2},
        {.type = EV_ABS, .code = ABS_X, .value = 1},
        {.type = EV_SYN, .code = SYN_REPORT}
    };
    for (int i = 0; i < R1_QUEUE_CAPACITY; ++i) {
        assert(write(input[1], report, sizeof(report)) == sizeof(report));
        drain_while_clicking();
        assert(running && pending.count == 0);
    }
    struct input_event motion[] = {
        {.type = EV_REL, .code = REL_X, .value = 28},
        {.type = EV_SYN, .code = SYN_REPORT}
    };
    assert(write(input[1], motion, sizeof(motion)) == sizeof(motion));
    drain_while_clicking();
    assert(pending.count == 2 && pending.motion == 1);
    close(input[0]); close(input[1]); r1 = -1;
    pending = (struct r1_event_queue){0};
    puts("PASS: foreground controls cannot fill replay queue; pointer frames remain ordered");
}

static void application_resume_test(int changing, int switch_task, int missing_info)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = input[0]; input_writer = input[1];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    application_mode = running = 1;
    simulated_ms = 0; session_deadline = 20000;
    first_capture_ms = focus_ms = frame_copies = 0;
    inject_motion = 1; injected = captured_ready = 0;
    inject_back = inject_buttons = early_motion = 0;
    changing_frames = changing;
    missing_task_info = missing_info;
    changed_task = switch_task; task_switch_ms = 12600;
    pending = (struct r1_event_queue){0};
    controller_map_reset(&mapping);
    self = 20; previous = active_id = 10; previous_sub = 0;
    canvas = &test_canvas; canvas_snapshot = *canvas; canvas_size = sizeof(pixels);
    background = malloc(canvas_size); assert(background);
    memset(background, 255, canvas_size);
    cursor_repair_reset(&repair);
    assert(await_foreground(30, 40, 0) == 0);
    assert(first_capture_ms == 12000 && frame_copies > 1 && active_id == self);
    uint64_t observed_from = missing_info ? 18000 : switch_task ? task_switch_ms : first_capture_ms;
    /* Stable frame: normal 1.2s guard. Animation: bounded 6s fallback.
     * Neither reading time nor frames from a previous task count. */
    assert(focus_ms == observed_from + (changing ? 6000 : 1200));
    close(input[0]); close(input[1]); r1 = -1;
    free(background); background = NULL;
    application_mode = changing_frames = changed_task = missing_task_info = 0; task_switch_ms = 1500;
    pending = (struct r1_event_queue){0};
    puts("PASS: late movement waits for observed frames; animation fallback and task-switch timer remain bounded");
}

static void interrupted_poll_test(void)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = input[0];
    controller_map_reset(&mapping);
    mapping.down[304] = 1;
    c_down = 1;
    interrupt_poll = 1;
    reconnect_r1();
    assert(r1 == input[0] && fcntl(r1, F_GETFD) >= 0);
    assert(mapping.down[304] && c_down); /* No phantom disconnect or lost release. */
    close(input[0]); close(input[1]); r1 = -1; c_down = 0;
    controller_map_reset(&mapping);
    puts("PASS: interrupted poll preserves the live controller and held buttons");
}

static void native_handoff_test(int quick)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = input[0]; input_writer = input[1];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    touch_fd = input[0]; hardware_queue = 17;
    application_mode = running = 1;
    simulated_ms = 0; session_deadline = 22000;
    first_capture_ms = focus_ms = frame_copies = 0;
    inject_motion = 1; injected = quick; captured_ready = 0;
    inject_back = inject_buttons = early_motion = changed_task = changing_frames = 0;
    native_queued = 1; native_delivered = second_motion = 0; native_hold_until = quick ? 1 : 16000;
    sent_taps = bw_updates = async_gray_updates = update_queue = 0;
    pending = (struct r1_event_queue){0};
    controller_map_reset(&mapping);
    self = active_id = 20; previous = 10; previous_sub = 0;
    canvas = &test_canvas; canvas_snapshot = *canvas; canvas_size = sizeof(pixels);
    background = malloc(canvas_size); assert(background);
    memset(background, 255, canvas_size); memcpy(pixels, background, canvas_size);
    cursor_draw(pixels, canvas->scanline, 30, 40);
    cursor_repair_reset(&repair);
    cursor_repair_add(&repair, (cursor_rect){80, 100, 24, 32});
    if (quick) {
        /* The stock app drains the native tap during activation, before the
         * first 150ms poll. Controller backlog spans multiple bounded drains. */
        stale_reports = 600;
        struct input_event buttons[] = {
            {.type = EV_KEY, .code = KEY_VOLUMEUP, .value = 1},
            {.type = EV_KEY, .code = KEY_VOLUMEUP, .value = 0},
            {.type = EV_KEY, .code = BTN_RIGHT, .value = 1},
            {.type = EV_KEY, .code = BTN_RIGHT, .value = 0},
            {.type = EV_KEY, .code = BTN_LEFT, .value = 1},
            {.type = EV_KEY, .code = BTN_LEFT, .value = 0}
        };
        assert(write(input_writer, buttons, sizeof(buttons)) == sizeof(buttons));
        mapping.down[304] = c_down = circle_down = 1;
    }
    assert(yield_to_native_input(30, 40) == 0);
    assert(native_delivered && native_queued == 0 && sent_taps == 0);
    assert(first_capture_ms == 18000 && focus_ms == 19200 && active_id == self);
    assert(pending.count == 2 && repair.count == 0);
    assert(!stale_reports && !c_down && !circle_down && !mapping.down[304]);
    close(input[0]); close(input[1]); r1 = touch_fd = -1; hardware_queue = (mqd_t)-1;
    free(background); background = NULL;
    application_mode = 0; native_hold_until = 0; pending = (struct r1_event_queue){0};
    puts("PASS: modeled native handoff preserves queued input, discards held/quick-tap backlog and resumes only on later motion");
}

static void native_reclaim_race_test(int race, int resume)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = input[0]; input_writer = input[1];
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    touch_fd = input[0]; hardware_queue = 17;
    application_mode = running = 1;
    simulated_ms = 0; session_deadline = resume ? 22000 : 17000;
    first_capture_ms = focus_ms = frame_copies = 0;
    inject_motion = 1; injected = captured_ready = second_motion = 0;
    touch_race = race; touch_raced = reclaim_checks = 0;
    pending = (struct r1_event_queue){0};
    controller_map_reset(&mapping);
    self = 20; previous = active_id = 10; previous_sub = 0;
    canvas = &test_canvas; canvas_snapshot = *canvas; canvas_size = sizeof(pixels);
    background = malloc(canvas_size); assert(background);
    memset(background, 255, canvas_size);
    cursor_repair_reset(&repair);
    assert(await_foreground(30, 40, 0) == (resume ? 0 : -1));
    assert(touch_raced && first_capture_ms == 12000);
    if (resume) assert(focus_ms == 19200 && active_id == self && pending.count == 2);
    else assert(active_id == previous && !pending.count && !second_motion);
    close(input[0]); close(input[1]); r1 = touch_fd = -1; hardware_queue = (mqd_t)-1;
    free(background); background = NULL;
    application_mode = touch_race = touch_raced = 0;
    pending = (struct r1_event_queue){0};
    puts("PASS: touch racing capture/reclaim invalidates old motion; a fresh stable capture requires later movement");
}

static void native_controller_drain_test(void)
{
    int input[2];
    assert(pipe(input) == 0);
    r1 = touch_fd = input[0]; hardware_queue = 17;
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    running = 1; native_queued = 1;
    drain_while_clicking();
    native_queued = 0; stale_reports = 600;
    drain_while_clicking();
    assert(stale_reports == 344 && discard_r1 && !pending.count);
    drain_while_clicking();
    assert(stale_reports == 88 && discard_r1 && !pending.count);
    drain_while_clicking();
    assert(!stale_reports && !discard_r1 && !pending.count);
    for (int held = 0; held <= 1; ++held) {
        for (int long_return = 0; long_return <= 1; ++long_return) {
            running = 1; native_queued = 1;
            struct input_event ev = {.type = EV_KEY, .code = BTN_RIGHT, .value = 1};
            ev.input_event_sec = 1;
            assert(write(input[1], &ev, sizeof(ev)) == sizeof(ev));
            drain_while_clicking();
            assert(return_button.down && suppress_return);
            native_queued = held;
            drain_while_clicking();
            ev.value = 0;
            ev.input_event_sec = long_return ? 2 : 1;
            ev.input_event_usec = 70000;
            assert(write(input[1], &ev, sizeof(ev)) == sizeof(ev));
            drain_while_clicking();
            assert(running == !long_return && !return_button.down && !suppress_return);
        }
    }
    running = 1; native_queued = 0;
    drain_while_clicking();
    struct input_event before_touch = {.type = EV_KEY, .code = 311, .value = 1};
    before_touch.input_event_sec = 1;
    assert(write(input[1], &before_touch, sizeof(before_touch)) == sizeof(before_touch));
    drain_while_clicking();
    native_queued = 1;
    drain_while_clicking();
    native_queued = 0;
    drain_while_clicking();
    before_touch.value = 0; before_touch.input_event_usec = 70000;
    assert(write(input[1], &before_touch, sizeof(before_touch)) == sizeof(before_touch));
    drain_while_clicking();
    assert(running && !return_button.down && !suppress_return);
    running = 1; native_queued = 1;
    struct input_event dropped = {.type = EV_SYN, .code = SYN_DROPPED};
    assert(write(input[1], &dropped, sizeof(dropped)) == sizeof(dropped));
    drain_while_clicking();
    assert(!running); /* Lost Return state must still fail safely during touch. */
    close(input[0]); close(input[1]); r1 = touch_fd = -1; hardware_queue = (mqd_t)-1;
    native_queued = discard_r1 = 0;
    puts("PASS: native discard is bounded, suppresses short Return through finger-up, preserves long Return and rejects lost input");
}

static void native_gamepad_handoff_test(void)
{
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    r1 = touch_fd = input[0]; gpio_fd = output[1]; hardware_queue = 17;
    assert(fcntl(r1, F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(output[0], F_SETFL, O_NONBLOCK) == 0);
    application_mode = running = 1;
    simulated_ms = 1000;
    self = 20; previous = active_id = 10; previous_sub = 0;
    inject_motion = inject_buttons = inject_back = 0;
    controller_map_reset(&mapping);
    /* Establish M+B before touching; there is no later joystick input to
     * disambiguate the shared B/D codes (304/305). */
    struct input_event mode = {.type = EV_ABS, .code = ABS_X, .value = 1};
    assert(write(input[1], &mode, sizeof(mode)) == sizeof(mode));
    drain_while_clicking();
    assert(mapping.mode == CONTROLLER_GAMEPAD);
    uint64_t guard = mapping.guard;
    simulated_ms += 400;
    struct input_event press = {.type = EV_KEY, .code = 304, .value = 1};
    assert(write(input[1], &press, sizeof(press)) == sizeof(press));
    drain_while_clicking();
    assert(mapping.down[304]);
    native_queued = 1;
    press.code = 305; /* B was held before touch; D is pressed during touch. */
    assert(write(input[1], &press, sizeof(press)) == sizeof(press));
    drain_while_clicking();
    assert(mapping.mode == CONTROLLER_GAMEPAD && mapping.guard == guard);
    assert(!mapping.down[304] && !mapping.down[305]);
    native_queued = 0;
    drain_while_clicking();
    assert(!discard_r1);
    struct input_event releases[] = {
        {.type = EV_KEY, .code = 304, .value = 0},
        {.type = EV_KEY, .code = 305, .value = 0}
    };
    assert(write(input[1], releases, sizeof(releases)) == sizeof(releases));
    drain_while_clicking();
    struct input_event received[8];
    assert(read(output[0], received, sizeof(received)) < 0 && errno == EAGAIN);
    struct input_event fresh[] = {
        {.type = EV_KEY, .code = 304, .value = 1},
        {.type = EV_KEY, .code = 304, .value = 0},
        {.type = EV_KEY, .code = 305, .value = 1},
        {.type = EV_KEY, .code = 305, .value = 0}
    };
    assert(write(input[1], fresh, sizeof(fresh)) == sizeof(fresh));
    drain_while_clicking();
    assert(read(output[0], received, sizeof(received)) == sizeof(received));
    for (int i = 0; i < 8; ++i) {
        assert(received[i].type == (i % 2 ? EV_SYN : EV_KEY));
        assert(received[i].code == (i % 2 ? SYN_REPORT : i < 4 ? KEY_PAGEDOWN : KEY_BACK));
        assert(received[i].value == (i % 4 == 0));
    }
    assert(running && active_id == previous && !pending.count);
    close(input[0]); close(input[1]); close(output[0]); close(output[1]);
    r1 = touch_fd = gpio_fd = -1; hardware_queue = (mqd_t)-1; application_mode = 0;
    controller_map_reset(&mapping);
    puts("PASS: native handoff preserves M+B mappings for fresh B/D presses and discards overlapping releases without joystick input");
}

int main(void)
{
    iv.active = fake_active; iv.activate = fake_activate; iv.info = fake_info;
    iv.copy = fake_copy; iv.canvas = fake_canvas; iv.wait = fake_wait;
    iv.update = fake_update; iv.orientation = fake_zero;
    iv.inverted = fake_false; iv.get_caps = fake_zero;
    iv.update_pending = fake_pending; iv.send_event = fake_send_event;
    iv.transform_pointer = fake_transform;
    native_tap_test();
    cursor_init_mask();
    scenario(0, 1);
    scenario(KEY_POWER, 1);
    scenario(KEY_BACK, 1);
    scenario(0, 0);
    early_motion = 1;
    scenario(0, 1);
    early_motion = 0; inject_back = 1;
    scenario(0, 1);
    inject_back = 0; inject_buttons = 1; early_motion = 1;
    scenario(0, 1);
    scenario(KEY_POWER, 1);
    scenario(KEY_BACK, 1);
    changed_task = 1;
    scenario(0, 1);
    puts("PASS: addressed pointer down/up, second tap only original task, C/Back foreground, BW movement/grayscale handoff, queue drain, cache guard and timeout");
    application_idle_test();
    foreground_queue_test();
    application_resume_test(0, 0, 0);
    application_resume_test(1, 0, 0);
    application_resume_test(1, 1, 0);
    application_resume_test(0, 0, 1);
    interrupted_poll_test();
    native_handoff_test(0);
    native_handoff_test(1);
    for (int race = TOUCH_DURING_COPY; race <= TOUCH_DURING_ACTIVATION; ++race) {
        native_reclaim_race_test(race, 0);
        native_reclaim_race_test(race, 1);
    }
    native_controller_drain_test();
    native_gamepad_handoff_test();
}
