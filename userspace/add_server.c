#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "mini_binder.h"

static void interrupted(int signal_number)
{
    /* Exit closes the device even if the signal arrives before RECEIVE. */
    _exit(128 + signal_number);
}

static int install_signals(void)
{
    struct sigaction action = { 0 };

    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL);
}

int main(int argc, char **argv)
{
    char *end;
    unsigned long count;
    int fd;

    if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '9') {
        fprintf(stderr, "usage: add_server COUNT (1..1000000)\n");
        return 2;
    }
    errno = 0;
    count = strtoul(argv[1], &end, 10);
    if (errno || *end != '\0' || count == 0 || count > 1000000) {
        fprintf(stderr, "invalid request count: expected 1..1000000\n");
        return 2;
    }
    if (install_signals()) {
        perror("sigaction");
        return 3;
    }
    fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("transport error: open");
        return 3;
    }
    if (ioctl(fd, MB_IOC_REGISTER) < 0) {
        perror("transport error: register");
        close(fd);
        return 3;
    }
    puts("READY");
    if (fflush(stdout) == EOF) {
        perror("ready output");
        close(fd);
        return 3;
    }
    for (unsigned long i = 0; i < count; ++i) {
        struct mb_request request;
        struct mb_reply reply = { 0 };

        if (ioctl(fd, MB_IOC_RECEIVE, &request) < 0) {
            perror("transport error: receive");
            close(fd);
            return 3;
        }
        reply.id = request.id;
        if (request.a > UINT32_MAX - request.b)
            reply.status = -EOVERFLOW;
        else
            reply.result = request.a + request.b;
        if (ioctl(fd, MB_IOC_REPLY, &reply) < 0) {
            perror("transport error: reply");
            close(fd);
            return 3;
        }
        printf("event=replied request_id=%" PRIu64 " status=%" PRId32 "\n",
               (uint64_t)reply.id, (int32_t)reply.status);
        if (fflush(stdout) == EOF) {
            perror("reply event output");
            close(fd);
            return 3;
        }
    }
    if (close(fd) < 0) {
        perror("transport error: close");
        return 3;
    }
    return 0;
}
