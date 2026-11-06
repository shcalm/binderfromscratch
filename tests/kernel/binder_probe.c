#include <fcntl.h>
#include <linux/android/binder.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    struct binder_version version = { 0 };
    int fd = open("/dev/binderfs/binder", O_RDWR);

    if (fd < 0) {
        perror("open real binder");
        return 1;
    }
    if (ioctl(fd, BINDER_VERSION, &version) < 0) {
        perror("BINDER_VERSION");
        close(fd);
        return 1;
    }
    close(fd);
    if (version.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION)
        return 1;
    printf("BINDER_PROBE_PASS protocol=%d userspace_bits=%zu\n",
           version.protocol_version, sizeof(void *) * 8);
    return 0;
}
