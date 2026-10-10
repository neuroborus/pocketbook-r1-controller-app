/* Narrow setuid entry point for the stock reader account. No shell or paths
 * from the caller are accepted; the mapper itself is not setuid. */
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INSTALL_DIR "/ebrmain/r1-controller"
#define CONTROLLER_PATH INSTALL_DIR "/r1-controller"

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--start") && strcmp(argv[1], "--stop") &&
                     strcmp(argv[1], "--status") && strcmp(argv[1], "--version"))) {
        fputs("Usage: r1-launcher --start | --stop | --status | --version\n", stderr);
        return 2;
    }
    if ((getuid() != 0 && getuid() != 101) || geteuid() != 0) {
        fputs("R1 launcher requires its root-owned installation and reader account\n", stderr);
        return 1;
    }
    struct stat st;
    if (lstat(INSTALL_DIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 022) || lstat(CONTROLLER_PATH, &st) < 0 ||
        !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 ||
        (st.st_mode & 022)) {
        fputs("R1 installation is missing or has unsafe ownership/permissions\n", stderr);
        return 1;
    }
    if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0 || chdir("/") < 0) {
        perror("R1 launcher privileges");
        return 1;
    }
    sigset_t unblocked;
    if (sigemptyset(&unblocked) < 0 || sigprocmask(SIG_SETMASK, &unblocked, NULL) < 0) {
        perror("R1 launcher signal mask");
        return 1;
    }
    umask(077);
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0) { fputs("Cannot determine descriptor limit\n", stderr); return 1; }
    for (long fd = 3; fd < limit; ++fd) close((int)fd);
    char *args[] = {CONTROLLER_PATH, argv[1], NULL};
    char *env[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LANG=C", NULL};
    execve(CONTROLLER_PATH, args, env);
    perror("R1 launcher exec");
    return 1;
}
