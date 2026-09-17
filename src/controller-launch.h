#ifndef CONTROLLER_LAUNCH_H
#define CONTROLLER_LAUNCH_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Share the lock with historical probes so they cannot run over the mapper. */
#ifndef CONTROLLER_LOCK
#define CONTROLLER_LOCK "/tmp/r1-inkview-overlay-test.lock"
#endif
#ifndef CONTROLLER_LOG
#define CONTROLLER_LOG "/tmp/r1-controller.log"
#endif

enum { CONTROLLER_CHILD = -1 };
static int controller_ready_fd = -1;

static int controller_open_lock(int create)
{
    int fd = open(CONTROLLER_LOCK, O_RDWR | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_uid == geteuid() && st.st_nlink == 1)
        return fd;
    close(fd);
    errno = EPERM;
    return -1;
}

static int controller_log_open(void)
{
    /* A FIFO must not block open() before the regular-file check, including
     * when reopening the log during rotation in the running controller. */
    int fd = open(CONTROLLER_LOG, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd);
        return -1;
    }
    int result = dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0 ? -1 : 0;
    if (fd > STDERR_FILENO) close(fd);
    return result;
}

static int controller_lock(int record_pid)
{
    int fd = controller_open_lock(1);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        if (!record_pid) return fd;
        char pid[32];
        int len = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
        if (ftruncate(fd, 0) == 0 && write(fd, pid, (size_t)len) == len) return fd;
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

/* Acknowledge only after logging, lock/PID and signal handlers are ready.
 * Direct --run has no parent waiting for this notification. */
static int controller_report_ready(void)
{
    if (controller_ready_fd < 0) return 0;
    ssize_t n;
    do { n = write(controller_ready_fd, "R", 1); } while (n < 0 && errno == EINTR);
    close(controller_ready_fd);
    controller_ready_fd = -1;
    return n == 1 ? 0 : -1;
}

static void controller_rotate_log(void)
{
    static time_t checked;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec - checked < 60) return;
    checked = now.tv_sec;
    struct stat st;
    if (fstat(STDOUT_FILENO, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 2 * 1024 * 1024)
        return;
    fflush(stdout);
    fflush(stderr);
    if (rename(CONTROLLER_LOG, CONTROLLER_LOG ".1") == 0)
        (void)controller_log_open();
}

/* Reject malformed/stale PID records and other executables before signalling. */
static int controller_control(int stop_it)
{
    int fd = controller_open_lock(0);
    if (fd < 0) {
        if (errno == ENOENT) {
            puts("R1 controller is stopped");
            return 0;
        }
        perror("controller lock");
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        close(fd);
        puts("R1 controller is stopped");
        return 0;
    }
    if (errno != EWOULDBLOCK) {
        perror("controller lock");
        close(fd);
        return 1;
    }
    char text[32] = {0}, path[64], exe[512], own[512], *end;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0 || n == (ssize_t)sizeof(text) - 1) return 1;
    errno = 0;
    long pid = strtol(text, &end, 10);
    if (errno || end == text || pid <= 1 || pid > INT32_MAX ||
        (*end && strcmp(end, "\n")))
        return 1;
    snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
    ssize_t a = readlink(path, exe, sizeof(exe) - 1);
    ssize_t b = readlink("/proc/self/exe", own, sizeof(own) - 1);
    if (a <= 0 || b <= 0 || a == (ssize_t)sizeof(exe) - 1 ||
        b == (ssize_t)sizeof(own) - 1)
        return 1;
    exe[a] = 0;
    own[b] = 0;
    if (strcmp(exe, own)) {
        puts("Another controller/test owns the lock; not signalling it");
        return 1;
    }
    printf("R1 controller pid=%ld %s\n", pid, stop_it ? "stopping" : "running");
    return stop_it && kill((pid_t)pid, SIGTERM) < 0 ? 1 : 0;
}

static int controller_wait_ready(int fd, int timeout_ms)
{
    struct timespec started, now;
    if (timeout_ms < 0 || clock_gettime(CLOCK_MONOTONIC, &started) < 0) return 0;
    int remaining = timeout_ms;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    for (;;) {
        int result = poll(&p, 1, remaining);
        if (result > 0) {
            char reply = 0;
            ssize_t n = read(fd, &reply, 1);
            if (n >= 0) return n == 1 && reply == 'R';
            if (errno != EINTR) return 0;
        } else if (result == 0 || errno != EINTR) {
            return 0;
        }
        /* Signals do not fail a valid launch or restart its timeout. */
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
        int64_t elapsed = (int64_t)(now.tv_sec - started.tv_sec) * 1000 +
            (now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms) return 0;
        remaining = timeout_ms - (int)elapsed;
    }
}

/* Returns CONTROLLER_CHILD in the detached child, which continues in main.
 * The parent gets success only on an explicit ready byte, never on EOF. */
static int controller_start(int timeout_ms)
{
    /* Keep a failed child waitable: inherited SIG_IGN/SA_NOCLDWAIT could
     * otherwise release its PID before the parent's cancellation below. */
    struct sigaction child_action = {.sa_handler = SIG_DFL};
    if (sigemptyset(&child_action.sa_mask) < 0 ||
        sigaction(SIGCHLD, &child_action, NULL) < 0)
        return 1;
    int ready[2];
    if (pipe(ready) < 0) return 1;
    /* Logging redirects descriptors 0..2. Keep the notification pipe out of
     * that range even when the caller started us with closed standard I/O. */
    for (int i = 0; i < 2; ++i) {
        if (ready[i] > STDERR_FILENO) continue;
        int moved = fcntl(ready[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0) { close(ready[0]); close(ready[1]); return 1; }
        close(ready[i]);
        ready[i] = moved;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) { close(ready[0]); close(ready[1]); return 1; }
    if (child) {
        close(ready[1]);
        int ok = controller_wait_ready(ready[0], timeout_ms);
        close(ready[0]);
        if (ok) {
            printf("R1 controller started pid=%ld; waiting for USB/R1\n", (long)child);
            return 0;
        }
        /* This is our own unreaped child, so its PID cannot have been reused.
         * Also cancel a hung bootstrap: no delayed, unexpected launch later. */
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        fprintf(stderr, "R1 controller failed to start; check %s and file permissions\n", CONTROLLER_LOG);
        return 1;
    }
    close(ready[0]);
    controller_ready_fd = ready[1];
    if (setsid() < 0 || controller_log_open() < 0) _exit(1);
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) _exit(1);
    if (nullfd > STDIN_FILENO) close(nullfd);
    return CONTROLLER_CHILD;
}
#endif
