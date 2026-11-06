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

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long long parsed;

    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static void interrupted(int signal_number)
{
    /* No automatic CALL retry: remote execution may already have started. */
    _exit(128 + signal_number);
}

int main(int argc, char **argv)
{
    struct mb_call call = { 0 };
    struct sigaction action = { 0 };
    uint32_t a, b;
    int fd;

    if (argc != 3 || parse_u32(argv[1], &a) || parse_u32(argv[2], &b)) {
        fprintf(stderr, "invalid argument: use add_client U32 U32\n");
        return 2;
    }
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction");
        return 3;
    }
    call.a = a;
    call.b = b;
    fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("transport error: open");
        return 3;
    }
    if (ioctl(fd, MB_IOC_CALL, &call) < 0) {
        perror("transport error: call");
        close(fd);
        return 3;
    }
    if (close(fd) < 0) {
        perror("transport error: close");
        return 3;
    }
    if (call.status) {
        fprintf(stderr, "business error: status=%" PRId32 " (%s)\n",
                (int32_t)call.status,
                call.status == -EOVERFLOW ? "overflow" : "remote failure");
        return 2;
    }
    printf("result=%" PRIu32 "\n", (uint32_t)call.result);
    return 0;
}
