#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uid_t caller = 101, effective = 0;
static int unsafe_case, failure, progress, executions, closed_fds;
static uid_t test_getuid(void) { return caller; }
static uid_t test_geteuid(void) { return effective; }
static int test_lstat(const char *path, struct stat *st)
{
    int directory = !strcmp(path, "/ebrmain/r1-controller");
    assert(directory || !strcmp(path, "/ebrmain/r1-controller/r1-controller"));
    *st = (struct stat){.st_mode = directory ? S_IFDIR | 0755 : S_IFREG | 0700,
                       .st_uid = 0, .st_nlink = 1};
    if (unsafe_case == 1) { errno = ENOENT; return -1; }
    if (unsafe_case == 2) st->st_uid = 101;
    if (unsafe_case == 3) st->st_mode |= 0020;
    if (unsafe_case == 4) st->st_mode = S_IFLNK | 0777;
    if (unsafe_case == 5 && !directory) st->st_nlink = 2;
    if (unsafe_case == 6 && !directory) st->st_mode = S_IFDIR | 0700;
    return 0;
}
static int advance(int step)
{
    assert(progress + 1 == step);
    progress = step;
    if (failure == step) { errno = EPERM; return -1; }
    return 0;
}
static int test_setgroups(size_t n, const gid_t *groups) { assert(!n && !groups); return advance(1); }
static int test_setgid(gid_t gid) { assert(!gid); return advance(2); }
static int test_setuid(uid_t uid) { assert(!uid); return advance(3); }
static int test_chdir(const char *dir) { assert(!strcmp(dir, "/")); return advance(4); }
static int test_sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    assert(how == SIG_SETMASK && set && !old);
    assert(!sigismember(set, SIGTERM) && !sigismember(set, SIGINT) && !sigismember(set, SIGCHLD));
    return advance(5);
}
static mode_t test_umask(mode_t mask) { assert(mask == 077); return 0; }
static long test_sysconf(int name) { assert(name == _SC_OPEN_MAX); return failure == 6 ? -1 : 6; }
static int test_close(int fd) { assert(fd == 3 + closed_fds); ++closed_fds; return 0; }
static int test_execve(const char *path, char *const args[], char *const env[])
{
    assert(progress == 5 && closed_fds == 3);
    assert(!strcmp(path, "/ebrmain/r1-controller/r1-controller"));
    assert(!strcmp(args[0], path) && args[1] && !args[2]);
    assert(!strcmp(env[0], "PATH=/usr/bin:/bin:/usr/sbin:/sbin"));
    assert(!strcmp(env[1], "LANG=C") && !env[2]);
    ++executions;
    errno = ENOENT; /* Returning from exec is an error; never execute a real mapper. */
    return -1;
}
#define getuid test_getuid
#define geteuid test_geteuid
#define lstat test_lstat
#define setgroups test_setgroups
#define setgid test_setgid
#define setuid test_setuid
#define chdir test_chdir
#define sigprocmask test_sigprocmask
#define umask test_umask
#define sysconf test_sysconf
#define close test_close
#define execve test_execve
#define main launcher_main
#include "../src/r1-launcher.c"
#undef main

static int invoke(char *action)
{
    char *args[] = {"r1-launcher", action, NULL};
    progress = executions = closed_fds = 0;
    return launcher_main(action ? 2 : 1, args);
}
int main(void)
{
    assert(invoke(NULL) == 2);
    assert(invoke("--run") == 2 && !executions);
    assert(invoke("--probe") == 2 && !executions);
    assert(invoke("/bin/sh") == 2 && !executions);
    char *extra[] = {"r1-launcher", "--start", "extra", NULL};
    assert(launcher_main(3, extra) == 2 && !executions);
    caller = 99; assert(invoke("--start") == 1 && !progress);
    caller = 101; effective = 101; assert(invoke("--start") == 1 && !progress);
    effective = 0;
    for (unsafe_case = 1; unsafe_case <= 6; ++unsafe_case)
        assert(invoke("--start") == 1 && !progress && !executions);
    unsafe_case = 0;
    for (failure = 1; failure <= 6; ++failure)
        assert(invoke("--start") == 1 && !executions);
    failure = 0;
    char *actions[] = {"--start", "--stop", "--status", "--version"};
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        assert(invoke(actions[i]) == 1 && executions == 1);
    caller = 0; assert(invoke("--version") == 1 && executions == 1);
    puts("PASS: restricted launcher arguments/callers/files, privilege failures, fixed executable, clean environment and descriptors; no root needed");
}
