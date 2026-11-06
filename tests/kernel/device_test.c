#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "mini_binder.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "device_test line=%d errno=%d\n", __LINE__, errno); \
        exit(1); \
    } \
} while (0)

int main(void)
{
    struct mb_query first, second, alias_query, final;
    int a = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    int b, alias, status;
    pid_t child;
    void *inaccessible;

    _Static_assert(sizeof(struct mb_query) == 24, "query ABI size");
    CHECK(a >= 0);
    CHECK(ioctl(a, MB_IOC_QUERY, &first) == 0);
    CHECK(first.version == MB_ABI_VERSION);
    b = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(b >= 0);
    CHECK(ioctl(b, MB_IOC_QUERY, &second) == 0);
    CHECK(first.open_id != second.open_id);
    CHECK(second.live_contexts == first.live_contexts + 1);
    alias = dup(b);
    CHECK(alias >= 0);
    CHECK(close(b) == 0);
    CHECK(ioctl(alias, MB_IOC_QUERY, &alias_query) == 0);
    CHECK(alias_query.open_id == second.open_id);
    CHECK(alias_query.live_contexts == second.live_contexts);

    errno = 0;
    CHECK(ioctl(a, _IO('m', 99), &final) == -1 && errno == ENOTTY);
    errno = 0;
    CHECK(ioctl(a, _IOR('m', 0, __u32), &final) == -1 && errno == ENOTTY);
    errno = 0;
    CHECK(ioctl(a, MB_IOC_QUERY, (void *)1) == -1 && errno == EFAULT);
    inaccessible = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(inaccessible != MAP_FAILED);
    errno = 0;
    CHECK(ioctl(a, MB_IOC_QUERY, inaccessible) == -1 && errno == EFAULT);
    CHECK(munmap(inaccessible, 4096) == 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int own;
        errno = 0;
        CHECK(ioctl(a, MB_IOC_QUERY, &final) == -1 && errno == EPERM);
        own = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
        CHECK(own >= 0);
        CHECK(ioctl(own, MB_IOC_QUERY, &final) == 0);
        CHECK(close(own) == 0);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(alias) == 0);
    {
        const unsigned char *region = mmap(NULL, MB_MAP_BYTES, PROT_READ,
                                           MAP_SHARED, a, 0);
        CHECK(region != MAP_FAILED);
        CHECK(region[0] == 0 && region[MB_MAP_BYTES - 1] == 0);
        CHECK(munmap((void *)region, MB_MAP_BYTES) == 0);
    }
    CHECK(ioctl(a, MB_IOC_QUERY, &final) == 0);
    CHECK(final.live_contexts == first.live_contexts);
    CHECK(close(a) == 0);
    printf("DEVICE_TEST_PASS open_id=%" PRIu64 "\n", (uint64_t)first.open_id);
    return 0;
}
