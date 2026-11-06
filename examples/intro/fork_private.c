#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    int result = -1;
    int status;
    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        result = 1 + 2;
        printf("child result=%d address=%p pid=%ld\n",
               result, (void *)&result, (long)getpid());
        fflush(stdout);
        _exit(0);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 1;
        }
    }
    printf("parent result=%d address=%p pid=%ld\n",
           result, (void *)&result, (long)getpid());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && result == -1
           ? 0 : 1;
}
