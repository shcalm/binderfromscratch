#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int result = -1, status;
    int signal_mode;
    pid_t child;

    if (argc != 2 || (strcmp(argv[1], "--exit") != 0 &&
                      strcmp(argv[1], "--signal") != 0)) {
        fprintf(stderr, "usage: fork_status --exit|--signal\n");
        return 2;
    }
    signal_mode = strcmp(argv[1], "--signal") == 0;
    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        if (signal_mode) {
            sigset_t signals;

            /* The controlled signal must not inherit an ignored/blocked state. */
            if (signal(SIGTERM, SIG_DFL) == SIG_ERR ||
                sigemptyset(&signals) < 0 || sigaddset(&signals, SIGTERM) < 0 ||
                sigprocmask(SIG_UNBLOCK, &signals, NULL) < 0 ||
                raise(SIGTERM) != 0)
                _exit(1);
            _exit(1);
        }
        result = 300;
        printf("child_result=%d\n", result);
        fflush(stdout);
        _exit(result);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 1;
        }
    }
    if (WIFEXITED(status)) {
        printf("event=exited exit_status=%d parent_result=%d\n",
               WEXITSTATUS(status), result);
        return !signal_mode && WEXITSTATUS(status) == (300 & 255) ? 0 : 1;
    }
    if (WIFSIGNALED(status)) {
        printf("event=signaled signal=%d parent_result=%d\n",
               WTERMSIG(status), result);
        return signal_mode && WTERMSIG(status) == SIGTERM ? 0 : 1;
    }
    return 1;
}
