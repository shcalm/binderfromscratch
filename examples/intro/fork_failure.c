#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    struct rlimit limit;
    pid_t child;
    int saved_errno;

    /* Root bypasses RLIMIT_NPROC; only this short-lived process drops UID. */
    if (getuid() == 0 && setuid(65534) < 0) {
        perror("setuid for resource-limit experiment");
        return 1;
    }
    if (getrlimit(RLIMIT_NPROC, &limit) < 0) {
        perror("getrlimit");
        return 1;
    }
    limit.rlim_cur = 0;
    if (setrlimit(RLIMIT_NPROC, &limit) < 0) {
        perror("setrlimit");
        return 1;
    }
    child = fork();
    saved_errno = errno;
    if (child == 0)
        _exit(1);
    if (child > 0) {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
        fprintf(stderr, "fork unexpectedly succeeded; check capabilities\n");
        return 1;
    }
    printf("event=fork_rejected errno=%d child_created=0\n", saved_errno);
    return saved_errno == EAGAIN ? 0 : 1;
}
