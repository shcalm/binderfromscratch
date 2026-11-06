#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t signal_seen;

static void on_signal(int signal_number)
{
    (void)signal_number;
    signal_seen = 1;
}

static int receive_marker(int fd, char expected)
{
    char marker;
    ssize_t count;

    do {
        count = read(fd, &marker, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1 && marker == expected ? 0 : -1;
}

static int wait_for_data(int fd, const sigset_t *wait_mask)
{
    fd_set reads;

    FD_ZERO(&reads);
    FD_SET(fd, &reads);
    return pselect(fd + 1, &reads, NULL, NULL, NULL, wait_mask);
}

static int reader(int data_fd, int control_fd, int retry,
                  const sigset_t *wait_mask)
{
    int result, saved_errno;
    ssize_t count;
    char value = '\0';

    printf("event=waiting pid=%ld\n", (long)getpid());
    if (write(control_fd, "R", 1) != 1)
        return 1;

    /* SIGUSR1 is blocked until this atomic unmask-and-wait operation. */
    result = wait_for_data(data_fd, wait_mask);
    saved_errno = errno;
    if (result != -1 || saved_errno != EINTR || !signal_seen) {
        fprintf(stderr, "expected signal interruption before any payload\n");
        return 1;
    }
    printf("event=interrupted return=%d errno=%d signal_seen=%d action=%s\n",
           result, saved_errno, (int)signal_seen, retry ? "retry" : "stop");
    if (write(control_fd, "I", 1) != 1)
        return 1;
    if (!retry) {
        printf("event=cancelled interruptions=1\n");
        return 0;
    }

    /* Waiting consumed no payload; this policy retries only an EINTR wait. */
    do {
        result = wait_for_data(data_fd, wait_mask);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror("pselect retry");
        return 1;
    }
    count = read(data_fd, &value, 1);
    printf("event=read_complete return=%ld value=%c interruptions=1\n",
           (long)count, value);
    return count == 1 && value == 'X' ? 0 : 1;
}

int main(int argc, char **argv)
{
    int data[2], control[2], retry, status, result = 0;
    struct sigaction action = { 0 };
    sigset_t blocked, wait_mask;
    pid_t child;

    if (argc != 2 || (strcmp(argv[1], "--retry") != 0 &&
                      strcmp(argv[1], "--stop") != 0)) {
        fprintf(stderr, "usage: interrupt_wait --retry|--stop\n");
        return 2;
    }
    retry = strcmp(argv[1], "--retry") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (sigaction(SIGUSR1, &action, NULL) < 0 ||
        sigprocmask(SIG_BLOCK, &blocked, &wait_mask) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal setup");
        return 1;
    }
    sigdelset(&wait_mask, SIGUSR1);
    if (pipe(data) < 0) {
        perror("data pipe");
        return 1;
    }
    if (pipe(control) < 0) {
        perror("control pipe");
        close(data[0]);
        close(data[1]);
        return 1;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        close(data[0]);
        close(data[1]);
        close(control[0]);
        close(control[1]);
        return 1;
    }
    if (child == 0) {
        close(data[1]);
        close(control[0]);
        result = reader(data[0], control[1], retry, &wait_mask);
        close(data[0]);
        close(control[1]);
        _exit(result);
    }
    close(data[0]);
    close(control[1]);
    if (receive_marker(control[0], 'R') < 0 ||
        kill(child, SIGUSR1) < 0 || receive_marker(control[0], 'I') < 0) {
        fprintf(stderr, "signal handshake failed\n");
        result = 1;
    } else if (retry && write(data[1], "X", 1) != 1) {
        perror("payload write");
        result = 1;
    }
    close(data[1]);
    close(control[0]);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 1;
        }
    }
    return result == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
