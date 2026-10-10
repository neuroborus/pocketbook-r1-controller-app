/* R1 Controller application for PocketBook Era Color and Magicsee R1.
 * ABI subset copied from pocketbook-inkview/610/inkview.h, lines 711-736,
 * 1266-1282, 1887, 2071, 2173, 2387-2391, 2702-2734, 2773.
 * Dynamic loading avoids linking the SDK's full dependency tree on the host.
 * Build with arm-linux-gnueabi-gcc (soft-float), not gnueabihf.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "cursor-raster.h"
#include "cursor-update-plan.h"
#include "cursor-repair.h"
#include "cursor-repair-seed.h"
#include "epdc-background-preserve.h"
#include "usb-state.h"
#include "touch-map.h"
#include "r1-event-queue.h"
#include "pointer-motion.h"
#include "return-button.h"
#include "controller-launch.h"
#include "controller-map.h"

static int application_mode;
static struct controller_map mapping;

#define TASK_NOHANDLER ((1 << 0) | (1 << 2) | (1 << 5) | (1 << 16))
#define TASK_COPYLASTFB (1 << 1)
#define TASK_FB_RGB24 (1 << 20)
#define UPD_NOIMPROVE (1 << 23)
#define UPD_CLEAR (1 << 17)
#define UPD_WAIT_AFTER (1 << 21)
#define UPD_NOREFRESH (1 << 18)
#define FRAME_INTERVAL_MS 20

static struct cursor_repair repair;

typedef struct {
    int width, height, scanline, depth;
    int clipx1, clipx2, clipy1, clipy2;
    uint8_t *addr;
} icanvas;

/* taskinfo_s from the same header, lines 1680-1696. */
typedef struct {
    int task, nsubtasks;
    unsigned int flags;
    int fbshmkey, fbshmsize;
    pid_t mainpid;
    char *appname;
    void *icon, *subtasks;
    int fbtempkey, rsv_2, rsv_3, rsv_4;
} taskinfo;

static struct {
    void (*init)(int);
    int (*current)(void);
    void (*active)(int *, int *);
    int (*activate)(int, int);
    void (*copy)(void);
    icanvas *(*canvas)(void);
    void (*update)(int, int, int, int, int);
    void (*wait)(void);
    bool (*inverted)(void);
    int (*get_caps)(void);
    void (*set_caps)(int);
    int (*list)(int *, int);
    taskinfo *(*info)(int);
    int (*orientation)(void);
    int (*update_pending)(void);
    int (*send_event)(int, int, int, int);
    int (*transform_pointer)(void *);
} iv;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t stop_signal;
static int self = -1, previous = -1, previous_sub = 0, r1 = -1;
static int presented;
static icanvas *canvas;
static icanvas canvas_snapshot;
static uint8_t *background;
static size_t canvas_size;
static int touch_fd = -1;
static mqd_t hardware_queue = (mqd_t)-1;
static int top_key_fd = -1;
static int gpio_fd = -1;
static struct return_button return_button;
static int c_down, circle_down;
static int discard_r1, suppress_return;
static struct input_absinfo touch_x, touch_y;
static struct r1_event_queue pending;
static uint64_t session_deadline;
static struct { int task, sub, x, y, down, sent; } repeat_click;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t input_time_ms(const struct input_event *ev)
{
    return ev->input_event_sec || ev->input_event_usec ?
        (uint64_t)ev->input_event_sec * 1000 + (uint64_t)ev->input_event_usec / 1000 : now_ms();
}

static void stop(int sig)
{
    stop_signal = sig;
    running = 0;
}

static void cleanup(void)
{
    if (r1 >= 0) {
        ioctl(r1, EVIOCGRAB, 0);
        close(r1);
        r1 = -1;
    }
    if (presented && previous > 0 && previous != self) {
        int active = -1, sub = 0;
        iv.active(&active, &sub);
        if (active == self) {
            int result = iv.activate(previous, previous_sub);
            printf("RESTORE task=%d sub=%d result=%d\n", previous, previous_sub, result);
            usleep(300000);
            iv.active(&active, &sub);
            printf("RESTORE_CHECK active=%d sub=%d\n", active, sub);
        } else {
            printf("RESTORE_SKIP already active=%d sub=%d\n", active, sub);
        }
    }
    presented = 0;
    if (hardware_queue != (mqd_t)-1) {
        mq_close(hardware_queue);
        hardware_queue = (mqd_t)-1;
    }
    if (touch_fd >= 0) { close(touch_fd); touch_fd = -1; }
    if (top_key_fd >= 0) { close(top_key_fd); top_key_fd = -1; }
    if (gpio_fd >= 0) { close(gpio_fd); gpio_fd = -1; }
    free(background);
    background = NULL;
}

static int load_iv(void)
{
    puts("LIBRARY /usr/lib/libinkview.so (firmware native; no SDK fallback)");
    void *lib = dlopen("/usr/lib/libinkview.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return -1;
    }
#define LOAD(field, name) do { \
    void *symbol = dlsym(lib, name); \
    if (!symbol) { fprintf(stderr, "Missing %s: %s\n", name, dlerror()); return -1; } \
    _Static_assert(sizeof(iv.field) == sizeof(symbol), "function pointer size"); \
    memcpy(&iv.field, &symbol, sizeof(symbol)); \
} while (0)
    LOAD(init, "InitInkview");
    LOAD(current, "GetCurrentTask");
    LOAD(active, "GetActiveTask");
    LOAD(activate, "SetActiveTask");
    LOAD(copy, "CopyActiveFb");
    LOAD(canvas, "GetCanvas");
    LOAD(update, "UpdateEx");
    LOAD(wait, "WaitForUpdateComplete");
    /* Exported by our verified 6.10 binary; declarations documented in
     * pocketbook-inkview/611/inkview.h:4395-4407. These set per-app capability,
     * not the global inversion setting. */
    LOAD(inverted, "IvGetScreenModeInversion");
    LOAD(get_caps, "IvGetAppCapability");
    LOAD(set_caps, "IvSetAppCapability");
    LOAD(list, "GetTaskList");
    LOAD(info, "GetTaskInfo");
    LOAD(orientation, "GetOrientation");
    LOAD(update_pending, "hw_is_update_pending"); /* inkinternal.h:967 */
    LOAD(send_event, "SendEventTo");
    LOAD(transform_pointer, "iv_transform_pointer");
#undef LOAD
    return 0;
}

static enum usb_state usb_configured(void)
{
    FILE *f = fopen("/sys/class/android_usb/android0/state", "r");
    if (!f) return USB_UNKNOWN;
    char state[40];
    int ok = fgets(state, sizeof(state), f) != NULL;
    fclose(f);
    if (!ok) return USB_UNKNOWN;
    return usb_parse_state(state);
}

static int open_input(const char *wanted, int flags, int grab)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return -1;
    struct dirent *e;
    int fd = -1;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[512], name[128];
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int ok = fgets(name, sizeof(name), f) != NULL;
        fclose(f);
        if (!ok) continue;
        name[strcspn(name, "\r\n")] = 0;
        if (strcmp(name, wanted)) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, flags);
        if (fd >= 0 && grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            perror("EVIOCGRAB");
            close(fd);
            fd = -1;
        }
        if (fd >= 0) printf("INPUT_CONNECTED name=%s path=%s grab=%d\n", wanted, path, grab);
        break;
    }
    closedir(dir);
    return fd;
}

static int open_r1(void)
{
    return open_input("Magicsee R1", O_RDONLY | O_NONBLOCK, 1);
}

static void reconnect_r1(void)
{
    static uint64_t next_try;
    if (r1 >= 0) {
        struct pollfd p = {.fd = r1, .events = POLLIN};
        int result = poll(&p, 1, 0);
        if (result < 0 && errno == EINTR) return;
        if (result >= 0 && !(p.revents & (POLLHUP | POLLERR | POLLNVAL))) return;
        (void)ioctl(r1,EVIOCGRAB,0); close(r1); r1=-1;
        memset(&return_button,0,sizeof(return_button)); c_down=circle_down=suppress_return=0;
        memset(&repeat_click,0,sizeof(repeat_click));
        memset(&pending,0,sizeof(pending)); controller_map_reset(&mapping);
        puts("R1_DISCONNECTED waiting for reconnect");
    }
    if (now_ms()<next_try) return;
    next_try=now_ms()+1000;
    r1=open_r1();
    if (r1>=0) { controller_map_reset(&mapping); puts("R1_RECONNECTED"); }
}

static int init_touch(void)
{
    touch_fd = open_input("fts_ts", O_RDWR | O_NONBLOCK, 0);
    if (touch_fd < 0 || ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &touch_x) < 0 ||
        ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &touch_y) < 0) {
        perror("touch init");
        return -1;
    }
    printf("TOUCH_RANGE x=%d..%d y=%d..%d\n", touch_x.minimum, touch_x.maximum,
        touch_y.minimum, touch_y.maximum);
    return 0;
}

/* Firmware 6.10 uses one /hwevent queue. hw_nextevent() consumes it only in
 * the active task (native libinkview, also inkinternal.h:439-446,979-981).
 * Observe its length WITHOUT receiving, injecting, flushing or unlinking:
 * returning focus is intended to let the stock app receive the original
 * gesture, including multitouch data. Physical validation is still pending. */
static int init_hardware_queue(void)
{
    hardware_queue = mq_open("/hwevent", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (hardware_queue == (mqd_t)-1) { perror("hardware queue open"); return -1; }
    struct mq_attr attr;
    if (mq_getattr(hardware_queue, &attr) < 0 || attr.mq_msgsize != 40) {
        fputs("Unsupported hardware event queue\n", stderr);
        mq_close(hardware_queue);
        hardware_queue = (mqd_t)-1;
        return -1;
    }
    puts("NATIVE_INPUT enabled; observing firmware queue without consuming events");
    return 0;
}

static void discard_controller_actions(void)
{
    discard_r1 = 1;
    suppress_return |= return_button.down;
    memset(&pending, 0, sizeof(pending));
    memset(&repeat_click, 0, sizeof(repeat_click));
    /* Native handoff does not change the controller mode or its switch guard. */
    memset(mapping.down, 0, sizeof(mapping.down));
    mapping.axes[0] = mapping.axes[1] = 1;
    c_down = circle_down = 0;
}

static int controller_return_event(const struct input_event *ev)
{
    suppress_return |= discard_r1;
    int action = return_button_event(&return_button, ev->value, input_time_ms(ev));
    if (suppress_return && action == 1) action = 0;
    if (!return_button.down) suppress_return = 0;
    return action;
}

static int native_input_busy(void)
{
    /* Latch observed activity until the controller backlog is drained, even
     * if the stock app empties /hwevent before the next observation. */
    if (hardware_queue == (mqd_t)-1) return 0;
    struct mq_attr attr;
    if (mq_getattr(hardware_queue, &attr) < 0) { perror("hardware queue state"); return -1; }
    if (attr.mq_curmsgs) { discard_controller_actions(); return 1; }
    /* An empty queue does not mean a held finger has finished its gesture. */
    unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
    unsigned bits = 8U * sizeof(unsigned long);
    if (touch_fd < 0) { fputs("Physical touch device unavailable\n", stderr); return -1; }
    if (ioctl(touch_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
        perror("physical touch state"); return -1;
    }
    int busy = (keys[BTN_TOUCH / bits] & (1UL << (BTN_TOUCH % bits))) != 0;
    if (busy) discard_controller_actions();
    return busy;
}

static int emit_input(int fd, int type, int code, int value)
{
    struct input_event ev = {0};
    ev.type = (uint16_t)type;
    ev.code = (uint16_t)code;
    ev.value = value;
    ssize_t n;
    do { n = write(fd, &ev, sizeof(ev)); } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(ev)) { perror("touch write"); return -1; }
    return 0;
}

static int __attribute__((unused)) emit_tap(int fd, int tx, int ty, int id)
{
    int rc = 0;
    rc |= emit_input(fd, EV_ABS, ABS_MT_SLOT, 0);
    rc |= emit_input(fd, EV_ABS, ABS_MT_TRACKING_ID, id);
    rc |= emit_input(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 64);
    rc |= emit_input(fd, EV_ABS, ABS_MT_POSITION_X, tx);
    rc |= emit_input(fd, EV_ABS, ABS_MT_POSITION_Y, ty);
    rc |= emit_input(fd, EV_KEY, BTN_TOUCH, 1);
    rc |= emit_input(fd, EV_SYN, SYN_REPORT, 0);
    usleep(70000);
    /* Always attempt release, including after a partial failed down sequence. */
    rc |= emit_input(fd, EV_ABS, ABS_MT_SLOT, 0);
    rc |= emit_input(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    rc |= emit_input(fd, EV_KEY, BTN_TOUCH, 0);
    rc |= emit_input(fd, EV_SYN, SYN_REPORT, 0);
    return rc;
}

static int emit_short_key(int fd, int code)
{
    int rc = emit_input(fd, EV_KEY, code, 1);
    rc |= emit_input(fd, EV_SYN, SYN_REPORT, 0);
    usleep(70000);
    rc |= emit_input(fd, EV_KEY, code, 0);
    rc |= emit_input(fd, EV_SYN, SYN_REPORT, 0);
    return rc;
}

static int send_native_key(int code)
{
    int fd = code == KEY_POWER ? top_key_fd : gpio_fd;
    if (fd < 0) { puts("KEY_SKIPPED device unavailable"); return -1; }
    unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
    unsigned bits = 8U * sizeof(unsigned long);
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    if (keys[code / bits] & (1UL << (code % bits))) {
        puts("KEY_SKIPPED physical key held"); return 0;
    }
    printf("NATIVE_KEY code=%d hold_ms=70\n", code);
    return emit_short_key(fd, code);
}

static int emit_inkview_tap(int task, int x, int y)
{
    /* SDK public event constants: POINTERDOWN=30, POINTERUP=29. Coordinates
     * are logical pixels; no guessing from mismatched evdev ABS ranges. */
    int down = iv.send_event(task, 30, x, y);
    usleep(70000);
    int up = iv.send_event(task, 29, x, y);
    printf("INKVIEW_TAP task=%d xy=%d,%d down=%d up=%d\n", task, x, y, down, up);
    return down > 0 && up > 0 ? 0 : -1;
}

static int emit_app_tap(int task, int x, int y)
{
    taskinfo *info = iv.info(task);
    if (!info || !info->appname) return -1;
    if (!strstr(info->appname, "/eink-reader.app"))
        return emit_inkview_tap(task, x, y);
    /* Firmware probe verified identity calibration in portrait. Reported
     * ABS maxima are NOT screen geometry on this fts_ts firmware. Don't
     * extrapolate that finding to rotated/calibrated configurations. */
    int event[16] = {30, 0, x, y, 64, 0, 0, 1};
    if (touch_fd < 0 || iv.orientation() != 0 || x < 0 || y < 0 ||
        x >= canvas->width || y >= canvas->height ||
        !iv.transform_pointer(event) || event[2] != x || event[3] != y) {
        puts("NATIVE_TAP_SKIPPED unsupported coordinate transform");
        return 0;
    }
    unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
    unsigned bits = 8U * sizeof(unsigned long);
    if (ioctl(touch_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    if (keys[BTN_TOUCH / bits] & (1UL << (BTN_TOUCH % bits))) {
        puts("NATIVE_TAP_SKIPPED physical touch held");
        return 0;
    }
    static int tracking_id = 20000;
    if (++tracking_id > 30000) tracking_id = 20000;
    printf("NATIVE_TAP task=%d screen/raw=%d,%d hold_ms=70 id=%d\n", task, x, y, tracking_id);
    return emit_tap(touch_fd, x, y, tracking_id);
}

static int wait_display(void)
{
    uint64_t until = now_ms() + 5000;
    while (running && iv.update_pending()) {
        if (now_ms() >= until) { puts("UPDATE_QUEUE_TIMEOUT"); return -1; }
        usleep(5000);
    }
    if (!running) return -1;
    iv.wait();
    return 0;
}

static uint32_t frame_hash(void)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < canvas_size; ++i)
        hash = (hash ^ canvas->addr[i]) * 16777619U;
    return hash;
}

static int refresh_canvas(void)
{
    icanvas *latest = iv.canvas();
    if (!latest || !latest->addr || latest->width != canvas_snapshot.width ||
        latest->height != canvas_snapshot.height || latest->depth != canvas_snapshot.depth ||
        latest->scanline != canvas_snapshot.scanline) {
        puts("CANVAS geometry changed; exit");
        return -1;
    }
    if (latest != canvas || latest->addr != canvas_snapshot.addr)
        printf("CANVAS_CHANGED old=%p new=%p\n", (void *)canvas_snapshot.addr, (void *)latest->addr);
    canvas = latest;
    canvas_snapshot = *latest;
    return 0;
}

static void update_rect(cursor_rect rect, int fast)
{
    /* fast=1: DU arrow; fast=2: legacy clearing; fast=3: async partial gray;
     * fast=0: sync initial/handoff.
     * Submit disjoint restore/draw regions as one batch, not the serial
     * grayscale waits per region that cause visible stalls. */
    int flags = (fast == 1 ? 1 : 2) | (fast ? UPD_NOREFRESH : UPD_WAIT_AFTER);
    /* Native 2.0 maps to AUTO/GU16; partial-only requests do not erase optical
     * arrow copies. UPD_CLEAR selects 2.1/GC16 for this rectangle only.
     * Do not set UPD_FULLSCREEN or change DU movement/conversion. */
    if (fast == 2) flags |= UPD_CLEAR;
    int previous_scope = r1_convert_scope;
    r1_convert_scope = fast == 1;
    iv.update(rect.x, rect.y, rect.w, rect.h, flags | UPD_NOIMPROVE);
    r1_convert_scope = previous_scope;
    if (!fast && wait_display() < 0) running = 0;
}

static int render_cursor(int old_x, int old_y, int x, int y, int fast)
{
    uint64_t start = now_ms();
    if (wait_display() < 0) return -1;
    if (refresh_canvas() < 0) return -1;
    cursor_rect rect = cursor_dirty(old_x, old_y, x, y, canvas->width, canvas->height);
    cursor_rect regions[CURSOR_UPDATE_LIMIT];
    int region_count = fast ? cursor_update_plan(old_x, old_y, x, y, canvas->width, canvas->height, regions) :
        cursor_regions(old_x, old_y, x, y, canvas->width, canvas->height, regions);
    if (region_count < 0) { puts("CURSOR_PLAN_FAILED"); return -1; }
    /* Preserve the input curve and silhouette geometry. Exposed intermediate
     * background tones need gray restoration; binary arrow regions retain DU. */
    int gray_background = cursor_needs_gray(background, canvas->scanline, rect);
    cursor_restore(canvas->addr, background, canvas->scanline, rect);
    cursor_draw(canvas->addr, canvas->scanline, x, y);
    int gray_restore_regions = 0;
    for (int i = 0; i < region_count; ++i) {
        int restore_gray = fast && cursor_repair_exposed(&repair,background,canvas->scanline,
            old_x,old_y,x,y,regions[i]);
        gray_restore_regions += restore_gray;
        update_rect(regions[i], fast);
    }
    if (fast) repair.last_activity=now_ms();
    int updated_area = 0;
    for (int i = 0; i < region_count; ++i) updated_area += cursor_rect_area(regions[i]);
    printf("FRAME_REGIONS count=%d area=%d plan=silhouette\n", region_count, updated_area);
    printf("GRAY_DEFER regions=%d debt=%d idle_ms=%d\n",gray_restore_regions,repair.count,CURSOR_REPAIR_IDLE_MS);
    printf("BACKGROUND_PRESERVED conversions=%lu\n",r1_convert_preserved);
    if (refresh_canvas() < 0) return -1;
    size_t mismatches = 0;
    for (int py = rect.y; py < rect.y + rect.h; ++py)
        for (int px = rect.x; px < rect.x + rect.w; ++px) {
            int sx = px - x, sy = py - y;
            int mask = sx >= 0 && sx < CURSOR_W && sy >= 0 && sy < CURSOR_H ? cursor_mask[sy][sx] : 0;
            size_t offset = (size_t)py * (size_t)canvas->scanline + (size_t)px * 3;
            for (int c = 0; c < 3; ++c) {
                uint8_t expected = mask ? (mask == 1 ? 255 : 0) : background[offset + (size_t)c];
                if (canvas->addr[offset + (size_t)c] != expected) ++mismatches;
            }
        }
    printf("FRAME x=%d y=%d rect=%d,%d,%d,%d elapsed_ms=%llu invert=%d caps=%d pixel_mismatches=%zu fast=%d gray_background=%d\n",
        x, y, rect.x, rect.y, rect.w, rect.h,
        (unsigned long long)(now_ms() - start), iv.inverted(), iv.get_caps(), mismatches, fast, gray_background);
    return 0;
}

static int flush_cursor_repair(void)
{
    if (!repair.count) return 0;
    if (wait_display()<0 || refresh_canvas()<0) return -1;
    uint64_t start=now_ms();
    cursor_rect regions[CURSOR_REPAIR_OUTPUT_LIMIT];
    int count=cursor_repair_plan(&repair,regions);
    if (count<=0) { puts("REPAIR_PLAN_FAILED"); return -1; }
    int area=0,legacy_area=0;
    for (int i=0;i<repair.count;++i) legacy_area+=cursor_rect_area(repair.rects[i]);
    printf("REPAIR_PLAN elapsed_ms=%llu\n",(unsigned long long)(now_ms()-start));
    for (int i=0;i<count;++i) area+=cursor_rect_area(regions[i]);
    size_t bytes=(size_t)area*3,offset=0,seeded=0;
    uint8_t *saved=malloc(bytes),*repair_canvas=canvas->addr;
    if (!saved) { puts("REPAIR_ALLOC_FAILED"); return -1; }
    /* Keep the exact current target, not the old application snapshot: the
     * current arrow must survive idle cleanup and be absent during handoff. */
    for (int i=0;i<count;++i) {
        cursor_rect r=regions[i];
        for (int y=r.y;y<r.y+r.h;++y) {
            size_t row=(size_t)r.w*3;
            memcpy(saved+offset,repair_canvas+(size_t)y*canvas->scanline+(size_t)r.x*3,row);
            offset+=row;
        }
        seeded+=cursor_repair_seed(&repair,repair_canvas,canvas->scanline,r);
    }
    for (int i=0;i<count;++i) update_rect(regions[i],1);
    if (wait_display()<0 || refresh_canvas()<0 || canvas->addr!=repair_canvas) {
        free(saved); puts("REPAIR_PRIME_ABORTED"); return -1;
    }
    offset=0;
    for (int i=0;i<count;++i) {
        cursor_rect r=regions[i];
        for (int y=r.y;y<r.y+r.h;++y) {
            size_t row=(size_t)r.w*3;
            memcpy(repair_canvas+(size_t)y*canvas->scanline+(size_t)r.x*3,saved+offset,row);
            offset+=row;
        }
    }
    free(saved);
    if (seeded) {
        for (int i=0;i<count;++i) update_rect(regions[i],3);
        if (wait_display()<0) return -1;
    }
    printf("GRAY_REPAIR regions=%d area=%d legacy_area=%d clear=0 seeded_pixels=%zu elapsed_ms=%llu completed=1\n",
        count,area,legacy_area,seeded,(unsigned long long)(now_ms()-start));
    cursor_repair_reset(&repair);
    return 0;
}

static int activate_checked(int task, int subtask)
{
    int result = iv.activate(task, subtask);
    printf("FOCUS_REQUEST task=%d sub=%d result=%d\n", task, subtask, result);
    uint64_t until = now_ms() + 2000;
    while (running && now_ms() < until) {
        int active = -1, sub = -1;
        iv.active(&active, &sub);
        if (active == task && sub == subtask) return 0;
        usleep(20000);
    }
    puts("FOCUS_TIMEOUT");
    return -1;
}

static int foreground_key(int code)
{
    int active = -1, sub = 0;
    iv.active(&active, &sub);
    taskinfo *info = active > 0 && active != self ? iv.info(active) : NULL;
    if (!info || info->mainpid <= 0 || kill(info->mainpid, 0) != 0) return -1;
    printf("FOREGROUND_KEY task=%d code=%d\n", active, code);
    if (code>=CONTROLLER_NAV) {
        int key=code-CONTROLLER_NAV;
        int down=iv.send_event(active,25,key,0);
        usleep(70000);
        int up=iv.send_event(active,26,key,0);
        return down>0 && up>0?0:-1;
    }
    return send_native_key(code);
}

static void reader_diagnostics(void)
{
    /* Only process identities/state, never book contents or command arguments. */
    DIR *dir = opendir("/proc");
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char path[512], exe[512];
        snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
        ssize_t n = readlink(path, exe, sizeof(exe) - 1);
        if (n < 0) continue;
        exe[n] = 0;
        if (!strstr(exe, "reader") && !strstr(exe, "bookshelf") && !strstr(exe, "/pocketbook")) continue;
        printf("READER_PROCESS pid=%ld exe=%s\n", pid, exe);
        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "State:", 6) || !strncmp(line, "VmRSS:", 6))
                printf("READER_STATUS pid=%ld %s", pid, line);
        fclose(f);
    }
    closedir(dir);
}

static void drain_while_clicking(void)
{
    if (application_mode) reconnect_r1();
    int native_busy = native_input_busy();
    if (native_busy < 0) { running = 0; return; }
    struct input_event ev;
    for (int batch = 0; batch < 256; ++batch) {
        /* A quick tap can finish before this first drain. Keep discarding
         * across bounded batches until an idle read proves the backlog empty. */
        ssize_t size = r1 >= 0 ? read(r1, &ev, sizeof(ev)) : 0;
        if (size != (ssize_t)sizeof(ev)) {
            if (!native_busy && (r1 < 0 || (size < 0 && errno == EAGAIN))) discard_r1 = 0;
            if (r1 >= 0 && !(size < 0 && (errno == EAGAIN || errno == EINTR))) {
                puts("INPUT_READ_FAILED during click"); running = 0;
            }
            return;
        }
        native_busy = native_input_busy();
        if (native_busy < 0) { running = 0; return; }
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            puts("SYN_DROPPED during click; exit"); running = 0; return;
        }
        /* Finger input wins. Discard overlapping controller actions instead
         * of replaying a delayed click/movement after the gesture. Long Return
         * remains available to stop, but short Back must not interrupt touch. */
        if (discard_r1) {
            if (ev.type == EV_KEY && (ev.code == BTN_RIGHT || ev.code == 311) &&
                controller_return_event(&ev) == 2) {
                puts("RETURN_LONG exit"); running = 0; return;
            }
            continue;
        }
        int mapped=controller_map_event(&mapping,&ev,now_ms());
        if (mapped) {
            if (foreground_key(mapped)<0) { puts("MAPPED_KEY_FAILED"); running=0; return; }
            continue;
        }
        if (ev.type == EV_KEY && (ev.code == BTN_RIGHT || ev.code == 311)) {
            int action = controller_return_event(&ev);
            if (action == 2) { puts("RETURN_LONG exit"); running = 0; return; }
            if (action == 1) {
                if (foreground_key(KEY_BACK) < 0) {
                    puts("BACK_FAILED exit safely"); running = 0; return;
                }
                puts("RETURN_BACK foreground");
            }
            continue;
        }
        if (ev.type == EV_KEY && ev.code == KEY_VOLUMEUP) {
            if (ev.value == 1) c_down = 1;
            if (ev.value == 0 && c_down) {
                c_down = 0;
                if (foreground_key(KEY_POWER) < 0) {
                    puts("C_FAILED exit safely"); running = 0; return;
                }
            }
            continue;
        }
        if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
            if (ev.value == 1) repeat_click.down = 1;
            if (!ev.value && repeat_click.down) {
                repeat_click.down = 0;
                int active = -1, sub = 0;
                if (repeat_click.task > 0) iv.active(&active, &sub);
                if (repeat_click.task > 0 && !repeat_click.sent && active == repeat_click.task && sub == repeat_click.sub) {
                    repeat_click.sent = 1;
                    puts("SECOND_CLICK same original task; no overlay focus switch");
                    if (emit_app_tap(active, repeat_click.x, repeat_click.y) < 0) { running = 0; return; }
                } else puts("SECOND_CLICK_SKIPPED target changed or pair already sent");
            }
            continue;
        }
        /* Controls above have already been delivered. Retain only pointer
         * motion and its frame boundaries for replay after taking focus back;
         * idle SYN/ABS/key events would fill the queue during a long read. */
        int pointer_event = ev.type == EV_REL &&
            (ev.code == REL_X || ev.code == REL_Y) && ev.value;
        int pointer_sync = ev.type == EV_SYN && ev.code == SYN_REPORT && pending.motion;
        if (!pointer_event && !pointer_sync) continue;
        if (r1_queue_push(&pending, ev) < 0) {
            puts("INPUT_QUEUE_FULL exit safely rather than lose button releases");
            running = 0;
            return;
        }
    }
}

static int next_input(struct input_event *ev)
{
    if (r1_queue_pop(&pending, ev)) return 1;
    return read(r1, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev);
}

static int await_foreground(int x,int y,uint32_t before_hash)
{
    int active=-1,sub=0;
    /* A stable frame can still be a loading screen. Leave the actual app
     * foreground until explicit pointer input, not a fixed screenshot timer. */
    puts("APP_FOREGROUND cursor hidden; move to resume; Return=Back, hold 1s+release=exit");
    uint64_t start = now_ms(), stable_since = start;
    uint64_t capture_started = start;
    uint32_t last_hash = 0;
    int last_task = -1, last_sub = -1, changed = 0, copied = 0;
    uint64_t next_notice = start;
    uint64_t next_diagnostics = start + 5000;
    int cache_notice = 0;
    while (running && now_ms() < session_deadline) {
        usleep(150000);
        drain_while_clicking();
        if (!running || usb_configured() == USB_CONNECTED) return -1;
        int native_busy = native_input_busy();
        if (native_busy < 0) return -1;
        if (native_busy || discard_r1) {
            copied = 0;
            continue;
        }
        iv.active(&active, &sub);
        taskinfo *info = active > 0 && active != self ? iv.info(active) : NULL;
        if (!info || info->mainpid <= 0 || kill(info->mainpid, 0) != 0) {
            if (application_mode) copied = 0;
            continue;
        }
        if (application_mode) {
            controller_rotate_log();
            /* While reading, do not copy/hash6MB every150ms. Start the
             * stable-frame acquisition only on explicit pointer movement. */
            if (!pending.motion) {
                copied = 0;
                continue;
            }
        }
        /* Idle reading is not observation time. Restart after reconnect or
         * loss of queued movement as well as on the first captured frame. */
        if (application_mode && (!copied || active != last_task || sub != last_sub))
            capture_started = now_ms();
        iv.copy();
        if (refresh_canvas() < 0) return -1;
        uint32_t hash = frame_hash();
        native_busy = native_input_busy();
        if (native_busy < 0) return -1;
        if (native_busy) { copied = 0; continue; }
        if (hash != last_hash || active != last_task || now_ms() >= next_notice) {
            printf("APP_FRAME task=%d app=%s hash=%08x elapsed_ms=%llu\n", active,
                info->appname ? info->appname : "(null)", hash,
                (unsigned long long)(now_ms() - start));
            next_notice = now_ms() + 5000;
        }
        if (active != last_task || sub != last_sub || hash != last_hash) stable_since = now_ms();
        changed |= active != previous || sub != previous_sub || hash != before_hash;
        last_task = active; last_sub = sub; last_hash = hash; copied = 1;
        /* A cached page is not proof the full reader is ready for interaction.
         * Do not steal focus back to a snapshot of its loading indicator. */
        int cached_reader = info->appname && strstr(info->appname, "eink-cache-reader.app");
        if (cached_reader) {
            if (!cache_notice) puts("WAIT_FULL_READER cache viewer active; Return remains Back");
            cache_notice = 1;
            if (now_ms() >= next_diagnostics) {
                reader_diagnostics();
                next_diagnostics = now_ms() + 10000;
            }
            continue;
        }
        if (!click_frame_ready(now_ms() - capture_started, now_ms() - stable_since,
                              changed, pending.motion) &&
            !(pending.motion && now_ms() - capture_started >= 6000)) continue;
        previous = last_task; previous_sub = last_sub;
        memcpy(background, canvas->addr, canvas_size);
        printf("CLICK_RESULT changed=%d task=%d sub=%d elapsed_ms=%llu hash=%08x\n",
            changed, previous, previous_sub, (unsigned long long)(now_ms() - start), last_hash);
        printf("CLICK_QUEUED events=%zu motion=%d\n", pending.count, pending.motion);
        /* Capture, hashing and copying take time. Revalidate immediately
         * before reclaim, and return focus if input races the activation. */
        iv.active(&active, &sub);
        if (active != last_task || sub != last_sub) { copied = 0; continue; }
        native_busy = native_input_busy();
        if (native_busy < 0) return -1;
        if (native_busy) { copied = 0; continue; }
        if (!running || now_ms() >= session_deadline || activate_checked(self, 0) < 0) return -1;
        if (wait_display() < 0 || refresh_canvas() < 0) return -1;
        native_busy = native_input_busy();
        if (native_busy < 0) return -1;
        if (native_busy) {
            if (activate_checked(previous, previous_sub) < 0) return -1;
            copied = 0;
            continue;
        }
        memcpy(canvas->addr, background, canvas_size);
        /* An unchanged click needs no redundant full-screen grayscale repaint. */
        if (changed) update_rect((cursor_rect){0, 0, canvas->width, canvas->height}, 0);
        return render_cursor(x, y, x, y, 0);
    }
    if (now_ms() >= session_deadline) puts("SESSION_TIMEOUT during click");
    return -1;
}

static int yield_to_native_input(int x, int y)
{
    taskinfo *info = previous > 0 && previous != self ? iv.info(previous) : NULL;
    if (!info || info->mainpid <= 0 || kill(info->mainpid, 0) != 0) return -1;
    if (wait_display() < 0 || refresh_canvas() < 0) return -1;
    cursor_rect area = cursor_dirty(x, y, x, y, canvas->width, canvas->height);
    cursor_restore(canvas->addr, background, canvas->scanline, area);
    uint32_t before_hash = frame_hash();
    /* Do not delay a live gesture with the optional trail-repair batch. The
     * foreground task restores its own screen. Never consume/replay its input. */
    cursor_repair_reset(&repair);
    discard_controller_actions();
    if (activate_checked(previous, previous_sub) < 0) return -1;
    puts("NATIVE_INPUT_HANDOFF cursor hidden; original hardware events left queued");
    return await_foreground(x, y, before_hash);
}

static int click_through(int x, int y, int key_code)
{
    int native_busy = native_input_busy();
    if (native_busy < 0) return -1;
    if (native_busy || discard_r1) return yield_to_native_input(x, y);
    int park=key_code==-1;
    if (park) key_code=0;
    int key_fd = key_code == KEY_POWER ? top_key_fd : gpio_fd;
    if (key_code && key_code<CONTROLLER_NAV && key_fd < 0) { puts("KEY_SKIPPED hardware unavailable"); return 0; }
    int tx, ty;
    int orientation = iv.orientation();
    if (touch_map(x + 2, y + 2, canvas->width, canvas->height, orientation,
        touch_x.minimum, touch_x.maximum, touch_y.minimum, touch_y.maximum, &tx, &ty) < 0)
        return -1;
    unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
    int state_fd = key_code ? key_fd : touch_fd;
    int state_key = key_code ? key_code : BTN_TOUCH;
    if (!park && key_code<CONTROLLER_NAV && ioctl(state_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) { perror("input state"); return -1; }
    unsigned bits = 8U * sizeof(unsigned long);
    if (!park && state_key<KEY_MAX && (keys[state_key / bits] & (1UL << (state_key % bits)))) {
        puts("ACTION_SKIPPED physical touch/key already down");
        return 0;
    }
    if (wait_display() < 0) return -1;
    if (refresh_canvas() < 0) return -1;
    cursor_rect area = cursor_dirty(x, y, x, y, canvas->width, canvas->height);
    cursor_restore(canvas->addr, background, canvas->scanline, area);
    uint32_t before_hash = frame_hash();
    /* Include current arrow in one bounded batch before handing the app back;
     * old trails must not survive an unchanged menu/click. */
    cursor_repair_hide(&repair,x,y,area);
    if (flush_cursor_repair()<0 || wait_display()<0) return -1;
    native_busy = native_input_busy();
    if (native_busy < 0) return -1;
    if (native_busy) return yield_to_native_input(x, y);
    if (activate_checked(previous, previous_sub) < 0) return -1;
    usleep(120000);
    int active = -1, sub = 0;
    iv.active(&active, &sub);
    if (!running || active != previous || sub != previous_sub) return -1;
    memset(&repeat_click, 0, sizeof(repeat_click));
    native_busy = native_input_busy();
    if (native_busy < 0) return -1;
    if (native_busy) return await_foreground(x, y, before_hash);
    if (park) {
        puts("CURSOR_PARKED move R1 to resume");
    } else if (key_code) {
        printf("HARDWARE_ACTION code=%d target=%d\n", key_code, previous);
        if (foreground_key(key_code) < 0) return -1;
    } else {
        printf("CLICK screen=%d,%d orientation=%d target=%d via=app-specific\n", x + 2, y + 2, orientation, previous);
        if (emit_app_tap(previous, x + 2, y + 2) < 0) return -1;
        repeat_click.task = previous; repeat_click.sub = previous_sub;
        repeat_click.x = x + 2; repeat_click.y = y + 2;
    }

    return await_foreground(x,y,before_hash);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("R1 Controller " R1_CONTROLLER_VERSION);
        return 0;
    }
    int start_requested = argc == 1 || (argc == 2 && !strcmp(argv[1], "--start"));
    if (start_requested) {
        if (geteuid() != 0) { fputs("R1 controller requires root SSH\n", stderr); return 1; }
        int result = controller_start(5000);
        if (result != CONTROLLER_CHILD) return result;
    }
    if (argc==2 && !strcmp(argv[1],"--stop")) return controller_control(1);
    if (argc==2 && !strcmp(argv[1],"--status")) return controller_control(0);
    application_mode = start_requested || (argc == 2 && !strcmp(argv[1], "--run"));
    controller_map_reset(&mapping);
    int probe = argc == 2 && strcmp(argv[1], "--probe") == 0;
    int smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
    int tasks = argc == 2 && strcmp(argv[1], "--tasks") == 0;
    int restore = argc == 3 && strcmp(argv[1], "--restore") == 0;
    int wait_usb = application_mode || (argc == 2 && strcmp(argv[1], "--wait-usb") == 0);
    if (!probe && !smoke && !tasks && !restore && !wait_usb) {
        fprintf(stderr, "Usage: %s --start | --stop | --status | --run | --version | --probe | --smoke | --tasks | --restore TASK | --wait-usb\n", argv[0]);
        return 2;
    }
    if (application_mode && geteuid()!=0) { puts("ERROR root access to input devices is required"); return 1; }
    int lock = controller_lock(application_mode);
    if (lock < 0) { perror("controller lock"); return 1; }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    atexit(cleanup);
    printf("START version=" R1_CONTROLLER_VERSION " pid=%ld mode=%s time=%lld monotonic_ms=%llu\n",
           (long)getpid(), application_mode ? "--run" : argv[1], (long long)time(NULL), (unsigned long long)now_ms());
    if (controller_report_ready() < 0) return 1;

    if (wait_usb) {
        puts("WAIT_USB no time limit; waiting for stable DISCONNECTED, retrying unavailable state");
        struct usb_disconnect_gate gate = {0};
        int previous_state = -2;
        uint64_t next_notice = 0;
        while (running) {
            enum usb_state state = usb_configured();
            uint64_t now = now_ms();
            if ((int)state != previous_state || now >= next_notice) {
                printf("USB_STATE monotonic_ms=%llu value=%s\n", (unsigned long long)now,
                    state == USB_CONNECTED ? "connected" : state == USB_DISCONNECTED ? "disconnected" : "unavailable");
                previous_state = state;
                next_notice = now + 60000;
            }
            if (usb_disconnect_ready(&gate, state, now)) break;
            usleep(200000);
        }
        if (!running) {
            printf("EXIT USB wait interrupted signal=%d\n", (int)stop_signal);
            return 1;
        }
        puts("USB_DISCONNECTED settling 3 seconds");
        for (int i = 0; running && i < 30; ++i) usleep(100000);
        uint64_t deadline = application_mode?UINT64_MAX:now_ms() + 120000;
        puts("WAIT_R1");
        while (running && (r1 = open_r1()) < 0 && now_ms() < deadline) sleep(1);
        if (!running || r1 < 0) {
            puts("EXIT R1 wait cancelled or timed out");
            return 1;
        }
    }

    if (load_iv() < 0) return 1;
    if ((wait_usb || probe) && init_touch() < 0) return 1;
    if (application_mode && init_hardware_queue() < 0) return 1;
    if (wait_usb || probe) {
        top_key_fd = open_input("pmu1736-powerkey", O_RDWR | O_NONBLOCK, 0);
        gpio_fd = open_input("gpio_keys", O_RDWR | O_NONBLOCK, 0);
        if (top_key_fd < 0) puts("C_DISABLED cannot open top hardware key");
    }
    iv.init(TASK_NOHANDLER | TASK_COPYLASTFB | TASK_FB_RGB24);
    if (!r1_epdc_preserve_ready()) {
        puts("EPDC_HOOK_UNAVAILABLE refusing visual test; build with --export-dynamic and supported firmware");
        return 1;
    }
    puts("EPDC_BACKGROUND_HOOK ready (process-local; scoped to cursor DU conversion)");
    printf("DISPLAY invert=%d caps_before=%d\n", iv.inverted(), iv.get_caps());
    iv.set_caps(iv.get_caps() | 1); /* APP_CAPABILITY_SUPPORT_SCREEN_INVERSION */
    printf("DISPLAY caps_after=%d\n", iv.get_caps());
    /* InkView installs signal handlers during initialization. */
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    self = iv.current();
    iv.active(&previous, &previous_sub);
    printf("TASKS self=%d previous=%d sub=%d\n", self, previous, previous_sub);
    if (tasks) {
        int ids[128];
        int count = iv.list(ids, 128);
        printf("TASK_COUNT %d\n", count);
        for (int i = 0; i < count && i < 128; ++i) {
            taskinfo *info = iv.info(ids[i]);
            if (info) printf("TASK id=%d pid=%ld alive=%d flags=%08x app=%s\n",
                info->task, (long)info->mainpid,
                info->mainpid > 0 && kill(info->mainpid, 0) == 0,
                info->flags, info->appname ? info->appname : "(null)");
        }
        return 0;
    }
    if (restore) {
        char *end;
        long target = strtol(argv[2], &end, 10);
        if (*end || target <= 0 || target > INT32_MAX) return 2;
        taskinfo *info = iv.info((int)target);
        if (!info || info->mainpid <= 0 || kill(info->mainpid, 0) != 0) {
            puts("RESTORE recorded ID has no live task; this restores an ID, not a working UI");
        }
        printf("RESTORE_REQUEST task=%ld app=%s result=%d\n", target,
            info && info->appname ? info->appname : "(null)", iv.activate((int)target, 0));
        usleep(300000);
        int active = -1, sub = 0;
        iv.active(&active, &sub);
        printf("RESTORE_CHECK active=%d sub=%d\n", active, sub);
        return active == target ? 0 : 1;
    }
    if (!probe) {
        taskinfo *info = NULL;
        uint64_t until = application_mode?UINT64_MAX:now_ms() + (wait_usb ? 30000 : 0);
        puts("CHECK_UNDERLYING waiting for a live stock task");
        do {
            if (application_mode && usb_configured()!=USB_DISCONNECTED) {
                puts("EXIT USB state changed before visual activation"); return 1;
            }
            iv.active(&previous, &previous_sub);
            info = previous > 0 && previous != self ? iv.info(previous) : NULL;
            if (info && info->mainpid > 0 && kill(info->mainpid, 0) == 0) break;
            info = NULL;
            if (wait_usb && running) usleep(200000);
        } while (running && now_ms() < until);
        if (!running || !info) {
            puts("EXIT underlying task is not alive; refusing visual activation");
            return 1;
        }
        printf("UNDERLYING task=%d pid=%ld app=%s\n", previous,
               (long)info->mainpid, info->appname ? info->appname : "(null)");
    }
    if (previous <= 0 || previous == self) {
        puts("EXIT cannot identify underlying task safely");
        return 1;
    }
    canvas = iv.canvas();
    if (!canvas || !canvas->addr || canvas->width < 100 || canvas->height < 100 ||
        canvas->width > 4096 || canvas->height > 4096 ||
        canvas->depth != 24 || canvas->scanline < canvas->width * 3 ||
        canvas->scanline > 16384) {
        puts("EXIT unsupported canvas");
        return 1;
    }
    if (application_mode && (canvas->width!=1264 || canvas->height!=1680 || iv.orientation()!=0)) {
        puts("EXIT this release supports portrait 1264x1680 only"); return 1;
    }
    canvas_snapshot = *canvas;
    canvas_size = (size_t)canvas->scanline * (size_t)canvas->height;
    printf("CANVAS %dx%d depth=%d stride=%d addr=%p hash_before=%08x\n",
           canvas->width, canvas->height, canvas->depth, canvas->scanline,
           (void *)canvas->addr, frame_hash());
    puts("COPY_ACTIVE begin");
    iv.copy();
    if (refresh_canvas() < 0) return 1;
    printf("COPY_ACTIVE done hash_after=%08x\n", frame_hash());
    if (probe) {
        int active = -1, sub = 0;
        iv.active(&active, &sub);
        printf("PROBE_DONE active=%d sub=%d no screen update requested\n", active, sub);
        return active == previous ? 0 : 1;
    }

    background = malloc(canvas_size);
    if (!background) return 1;
    memcpy(background, canvas->addr, canvas_size);
    if (!running) return 1;
    int x = canvas->width / 2, y = canvas->height / 2;
    int drawn_x = x, drawn_y = y;
    cursor_init_mask();
    session_deadline = application_mode ? UINT64_MAX : now_ms() + (smoke ? 3000 : 300000);
    int active = -1, sub = 0;
    int native_busy = native_input_busy();
    if (native_busy < 0) return 1;
    presented = 1;
    if (native_busy) {
        puts("START_IN_FOREGROUND native interaction in progress; move R1 afterward");
        if (await_foreground(x, y, 0) < 0) { cleanup(); return 1; }
    } else {
        int result = iv.activate(self, 0);
        printf("ACTIVATE result=%d\n", result);
        uint64_t activation_deadline = now_ms() + 3000;
        do {
            usleep(100000);
            iv.active(&active, &sub);
        } while (running && active != self && now_ms() < activation_deadline);
        if (!running || active != self) {
            printf("EXIT activation not confirmed active=%d\n", active);
            cleanup();
            return 1;
        }
        if (wait_display() < 0) { cleanup(); return 1; }
        if (refresh_canvas() < 0) { cleanup(); return 1; }
        native_busy = native_input_busy();
        if (native_busy < 0) { cleanup(); return 1; }
        if (native_busy) {
            if (activate_checked(previous, previous_sub) < 0 || await_foreground(x, y, 0) < 0) {
                cleanup(); return 1;
            }
        } else {
            printf("ACTIVATED addr=%p hash=%08x invert=%d caps=%d\n", (void *)canvas->addr,
                   frame_hash(), iv.inverted(), iv.get_caps());
            memcpy(canvas->addr, background, canvas_size);
            /* Present unchanged snapshot once with correct inversion and no line processing. */
            update_rect((cursor_rect){0, 0, canvas->width, canvas->height}, 0);
            if (render_cursor(x, y, x, y, 0) < 0) { cleanup(); return 1; }
        }
    }
    printf("MARKER_READY adaptive arrow; M+D moves; circle clicks; C top key; Return Back; hold Return 1s+release exits; timeout %d seconds\n",
           application_mode?0:smoke ? 3 : 300);
    uint64_t deadline = application_mode?UINT64_MAX:now_ms() + (smoke ? 3000 : 300000);
    if (application_mode) puts("APPLICATION_READY no session timeout; hold Return 1s and release to stop");
    session_deadline = deadline;
    struct pointer_motion motion = {0};
    int dx = 0, dy = 0;
    uint64_t next_reconnect = 0;
    uint64_t next_frame = now_ms() + FRAME_INTERVAL_MS;
    circle_down = 0;
    while (running && now_ms() < deadline) {
        if (application_mode) controller_rotate_log();
        if (!smoke && usb_configured() == 1) { puts("USB_RECONNECTED exit"); break; }
        iv.active(&active, &sub);
        if (active != self) {
            if (!application_mode) { puts("FOCUS_LOST exit"); break; }
            puts("FOCUS_LOST waiting in background");
            cursor_repair_reset(&repair);
            x=drawn_x; y=drawn_y; dx=dy=0; memset(&motion,0,sizeof(motion));
            if (await_foreground(x,y,0)<0) break;
            next_frame=now_ms()+FRAME_INTERVAL_MS; continue;
        }
        int native_busy = native_input_busy();
        if (native_busy < 0) break;
        if (native_busy || discard_r1) {
            x = drawn_x; y = drawn_y; dx = dy = 0;
            circle_down = 0;
            memset(&motion, 0, sizeof(motion));
            if (yield_to_native_input(x, y) < 0) break;
            next_frame = now_ms() + FRAME_INTERVAL_MS;
            continue;
        }
        if ((x != drawn_x || y != drawn_y) && now_ms() >= next_frame) {
            if (render_cursor(drawn_x, drawn_y, x, y, 1) < 0) break;
            drawn_x = x; drawn_y = y;
            next_frame = now_ms() + FRAME_INTERVAL_MS;
        }
        if (r1 < 0) {
            if (!smoke && now_ms() >= next_reconnect) {
                r1 = open_r1();
                next_reconnect = now_ms() + 1000;
            }
            usleep(100000);
            continue;
        }
        struct pollfd pfd = {.fd = r1, .events = POLLIN};
        int ready = pending.count ? 1 : poll(&pfd, 1, 20);
        if (ready < 0 && errno == EINTR) continue;
        /* A touch may have arrived during rendering or poll. Yield before
         * processing a competing controller action or starting idle repair. */
        native_busy = native_input_busy();
        if (native_busy < 0) break;
        if (native_busy) continue;
        if (ready < 0 || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            ioctl(r1, EVIOCGRAB, 0);
            close(r1);
            r1 = -1;
            dx = dy = 0;
            circle_down = 0;
            c_down = 0;
            memset(&return_button, 0, sizeof(return_button));
            suppress_return = 0;
            memset(&pending, 0, sizeof(pending));
            memset(&motion, 0, sizeof(motion));
            controller_map_reset(&mapping);
            puts("R1_DISCONNECTED waiting for reconnect");
            if (application_mode && click_through(drawn_x,drawn_y,-1)<0) break;
            continue;
        }
        if (!ready) {
            if (cursor_repair_due(&repair,now_ms(),0,x!=drawn_x || y!=drawn_y) &&
                flush_cursor_repair()<0) break;
            continue;
        }
        struct input_event ev;
        int batch = 0;
        while (running && batch++ < 256) {
            native_busy = native_input_busy();
            if (native_busy < 0) { running = 0; break; }
            if (native_busy || discard_r1 || !next_input(&ev)) break;
            int mapped=controller_map_event(&mapping,&ev,now_ms());
            if (mapped) {
                x=drawn_x; y=drawn_y; dx=dy=0; memset(&motion,0,sizeof(motion));
                if (click_through(x,y,mapped)<0) { running=0; break; }
                next_frame=now_ms()+FRAME_INTERVAL_MS; continue;
            }
            if (ev.type == EV_KEY && (ev.code == BTN_RIGHT || ev.code == 311)) {
                int action = controller_return_event(&ev);
                if (action == 2) { puts("RETURN_LONG exit"); running = 0; break; }
                if (action == 1) {
                    x = drawn_x; y = drawn_y; dx = dy = 0;
                    memset(&motion, 0, sizeof(motion));
                    if (click_through(x, y, KEY_BACK) < 0) { running = 0; break; }
                    next_frame = now_ms() + FRAME_INTERVAL_MS;
                }
                continue;
            }
            if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == KEY_VOLUMEUP)) {
                int *down = ev.code == BTN_LEFT ? &circle_down : &c_down;
                if (ev.value == 1) *down = 1;
                if (ev.value == 0 && *down) {
                    *down = 0;
                    /* Click the last visible hotspot, not queued unseen movement. */
                    x = drawn_x; y = drawn_y; dx = dy = 0;
                    memset(&motion, 0, sizeof(motion));
                    if (click_through(x, y, ev.code == KEY_VOLUMEUP ? KEY_POWER : 0) < 0) { running = 0; break; }
                    next_frame = now_ms() + FRAME_INTERVAL_MS;
                }
            }
            if (ev.type == EV_REL && ev.code == REL_X) dx += ev.value;
            if (ev.type == EV_REL && ev.code == REL_Y) dy += ev.value;
            if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                puts("SYN_DROPPED exit"); running = 0; break;
            }
            if (ev.type != EV_SYN || ev.code != SYN_REPORT || (!dx && !dy)) continue;
            repair.last_activity=now_ms();
            int raw_dx = dx, raw_dy = dy;
            /* Use event timestamps so batched/queued reports don't invent a
             * long hold. Do not mix their clock with our monotonic deadline. */
            uint64_t event_ms = input_time_ms(&ev);
            pointer_step(&motion, event_ms, &dx, &dy);
            printf("MOTION time_ms=%llu raw=%d,%d scaled=%d,%d\n",
                (unsigned long long)event_ms, raw_dx, raw_dy, dx, dy);
            x += dx; y += dy;
            dx = dy = 0;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > canvas->width - CURSOR_W) x = canvas->width - CURSOR_W;
            if (y > canvas->height - CURSOR_H) y = canvas->height - CURSOR_H;
        }
    }
    puts("EXIT controller session finished");
    /* Run before InkView's own atexit hooks discard task state. */
    cleanup();
    return 0;
}
