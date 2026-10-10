#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
static int test_truncate(int fd, off_t length);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);
static int test_clock(clockid_t id, struct timespec *time);
#define ftruncate test_truncate
#define poll test_poll
#define clock_gettime test_clock
static char test_lock[256];
#define CONTROLLER_LOCK test_lock
#define CONTROLLER_LOG "controller.log"
#define main controller_program_main
#include "../src/r1-controller.c"
#undef main
#undef ftruncate
#undef poll
#undef clock_gettime
#include <assert.h>

static int fail_pid_write;
static int interruptions, interruption_ms, poll_timeouts;
static int64_t simulated_ms = -1;
static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (interruptions) {
        --interruptions;
        if (simulated_ms >= 0) simulated_ms += interruption_ms;
        errno = EINTR;
        return -1;
    }
    int result = poll(fds, count, timeout);
    if (result == 0 && timeout > 0) ++poll_timeouts;
    return result;
}
static int test_clock(clockid_t id, struct timespec *time)
{
    if (simulated_ms < 0) return clock_gettime(id, time);
    time->tv_sec = (time_t)(simulated_ms / 1000);
    time->tv_nsec = (long)(simulated_ms % 1000) * 1000000;
    return 0;
}
static int test_truncate(int fd, off_t length)
{
    if (fail_pid_write) { errno = ENOSPC; return -1; }
    return ftruncate(fd, length);
}

enum launch_case { LAUNCH_OK, FAIL_LOG, FAIL_LOG_FIFO, FAIL_LOCK, FAIL_PID, NO_ACK, HUNG_CHILD };

static void launch_test(enum launch_case test)
{
    int held = -1;
    poll_timeouts = 0;
    fail_pid_write = test == FAIL_PID;
    if (test == FAIL_LOG) assert(mkdir(CONTROLLER_LOG, 0700) == 0);
    if (test == FAIL_LOG_FIFO) assert(mkfifo(CONTROLLER_LOG, 0600) == 0);
    if (test == FAIL_LOCK) {
        held = controller_lock(1);
        assert(held >= 0);
    }
    int result = controller_start(test == HUNG_CHILD ? 0 : 1000);
    if (result == CONTROLLER_CHILD) {
        if (held >= 0) close(held);
        if (test == HUNG_CHILD) for (;;) pause();
        if (test == NO_ACK) _exit(0); /* EOF is not readiness, even on exit 0. */
        assert(test != FAIL_LOG && test != FAIL_LOG_FIFO); /* Fails before returning into caller. */
        int lock = controller_lock(1);
        if (lock < 0) _exit(1);
        assert(fputs("LAUNCH_READY\n", stdout) >= 0 && fflush(stdout) == 0);
        assert(controller_report_ready() == 0);
        for (;;) pause();
    }
    if (test == LAUNCH_OK) {
        assert(result == 0 && controller_control(0) == 0);
        FILE *log = fopen(CONTROLLER_LOG, "r");
        char line[32];
        assert(log && fgets(line, sizeof(line), log) && !strcmp(line, "LAUNCH_READY\n"));
        assert(fclose(log) == 0);
        assert(controller_control(1) == 0);
        int status;
        assert(waitpid(-1, &status, 0) > 0);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        assert(controller_control(0) == 0);
    } else {
        assert(result == 1);
        errno = 0;
        assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    }
    if (held >= 0) close(held);
    fail_pid_write = 0;
    if (test == FAIL_LOG) assert(rmdir(CONTROLLER_LOG) == 0);
    else if (unlink(CONTROLLER_LOG) < 0) assert(errno == ENOENT);
}

static void closed_stdio_test(void)
{
    int report[2];
    assert(pipe(report) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(report[0]);
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        launch_test(LAUNCH_OK);
        assert(write(report[1], "R", 1) == 1);
        _exit(0);
    }
    close(report[1]);
    char reply = 0;
    ssize_t n = read(report[0], &reply, 1);
    close(report[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(n == 1 && reply == 'R' && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS: detached launch works with all standard descriptors closed");
}

static void interrupted_wait_test(void)
{
    int ready[2];
    assert(pipe(ready) == 0);
    assert(write(ready[1], "R", 1) == 1);
    simulated_ms = 1000; interruption_ms = 25; interruptions = 1;
    assert(controller_wait_ready(ready[0], 50) == 1 && interruptions == 0);
    assert(write(ready[1], "R", 1) == 1);
    simulated_ms = 1000; interruptions = 2;
    assert(controller_wait_ready(ready[0], 50) == 0 && simulated_ms == 1050);
    simulated_ms = -1;
    assert(controller_wait_ready(ready[0], 0) == 1); /* Timed-out wait did not consume R. */
    close(ready[0]); close(ready[1]);
    puts("PASS: ready wait retries EINTR without extending the deadline; virtual time only");
}

static int inherited_sigchld_test(void)
{
    struct sigaction saved, inherited = {0}, actual;
    assert(sigaction(SIGCHLD, NULL, &saved) == 0);
    assert(sigemptyset(&inherited.sa_mask) == 0);
    int retained = 1;
    for (int i = 0; i < 2; ++i) {
        inherited.sa_handler = i ? SIG_DFL : SIG_IGN;
        inherited.sa_flags = i ? SA_NOCLDWAIT : 0;
        assert(sigaction(SIGCHLD, &inherited, NULL) == 0);
        launch_test(NO_ACK);
        assert(sigaction(SIGCHLD, NULL, &actual) == 0);
        /* A failed bootstrap must remain our child until waitpid reaps it;
         * neither inherited auto-reaping mode may release its PID early. */
        retained &= actual.sa_handler == SIG_DFL && !(actual.sa_flags & SA_NOCLDWAIT);
    }
    assert(sigaction(SIGCHLD, &saved, NULL) == 0);
    return retained;
}

int main(void)
{
    char *version_argv[] = {"r1-controller", "--version", NULL};
    assert(controller_program_main(2, version_argv) == 0);
    interrupted_wait_test();
    char directory[sizeof(test_lock) - 6];
    const char *temporary = getenv("TMPDIR");
    if (!temporary || !*temporary) temporary = "/tmp";
    int length = snprintf(directory, sizeof(directory), "%s/r1-launch-test-XXXXXX", temporary);
    assert(length > 0 && (size_t)length < sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(test_lock, sizeof(test_lock), "%s/lock", directory) > 0);
    assert(controller_control(0) == 0);
    assert(mkfifo(test_lock, 0600) == 0);
    assert(controller_open_lock(0) == -1); /* Never use a non-regular lock. */
    assert(unlink(test_lock) == 0);
    int fd = controller_open_lock(1);
    assert(fd >= 0 && controller_control(0) == 0);

    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(fd);
        close(ready[0]);
        int lock = controller_open_lock(0);
        assert(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
        assert(dprintf(lock, "%ld\n", (long)getpid()) > 0);
        assert(write(ready[1], "!", 1) == 1);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    char notice;
    assert(read(ready[0], &notice, 1) == 1);
    close(ready[0]);
    assert(controller_control(0) == 0);
    const char *bad[] = {"", "0\n", "-1\n", "2\ngarbage", "9999999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
        size_t len = strlen(bad[i]);
        assert(write(fd, bad[i], len) == (ssize_t)len);
        assert(controller_control(1) == 1 && kill(child, 0) == 0);
    }
    assert(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    assert(dprintf(fd, "%ld\n", (long)child) > 0);
    assert(controller_control(1) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(controller_control(0) == 0);
    close(fd);
    assert(unlink(test_lock) == 0);
    puts("PASS: lock validation, status, malformed PID rejection and targeted stop; no sleeps");

    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0 && chdir(directory) == 0);
    launch_test(LAUNCH_OK);
    launch_test(FAIL_LOG);
    launch_test(FAIL_LOCK);
    launch_test(FAIL_PID);
    launch_test(NO_ACK);
    launch_test(HUNG_CHILD);
    closed_stdio_test();
    int children_retained = inherited_sigchld_test();
    launch_test(FAIL_LOG_FIFO);
    assert(unlink(test_lock) == 0);
    assert(fchdir(cwd) == 0); close(cwd);
    assert(rmdir(directory) == 0);
    assert(poll_timeouts == 0); /* A FIFO is rejected immediately, not by the parent timeout. */
    puts("PASS: invalid FIFO log path fails without blocking; temporary files removed");
    assert(children_retained);
    puts("PASS: inherited SIGCHLD settings cannot auto-reap the bootstrap child");
    puts("PASS: start confirms ready, reports log/lock/PID/early-exit failures, cancels timeout; no sleeps or root needed");
}
