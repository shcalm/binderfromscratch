#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int descriptors[2], saved_errno;
    ssize_t count;
    char value = '\0';

    if (pipe(descriptors) < 0) {
        perror("pipe");
        return 1;
    }
    errno = EDOM;
    count = write(descriptors[1], "X", 1);
    saved_errno = errno;
    printf("event=write_success return=%ld errno=%d decision=%s\n",
           (long)count, saved_errno, count == 1 ? "success" : "failure");
    if (count != 1) {
        close(descriptors[0]);
        close(descriptors[1]);
        return 1;
    }
    count = read(descriptors[0], &value, 1);
    printf("event=payload_read return=%ld value=%c\n", (long)count, value);
    close(descriptors[0]);
    close(descriptors[1]);
    if (count != 1 || value != 'X')
        return 1;

    count = write(-1, "X", 1);
    saved_errno = errno;
    printf("event=write_failure return=%ld errno=%d decision=stop\n",
           (long)count, saved_errno);
    return count == -1 && saved_errno == EBADF ? 0 : 1;
}
