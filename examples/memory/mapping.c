#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct view {
    void *reservation;
    volatile uint32_t *data;
    size_t bytes;
};

struct reply {
    uintptr_t address;
    uint32_t first;
    uint32_t last;
};

static struct timespec deadline;
static volatile sig_atomic_t interrupted;

static void on_signal(int number)
{
    interrupted = number;
}

static int remaining_ms(void)
{
    struct timespec now;
    int64_t milliseconds;

    if (interrupted) {
        errno = EINTR;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    milliseconds = (deadline.tv_sec - now.tv_sec) * INT64_C(1000)
                   + (deadline.tv_nsec - now.tv_nsec) / 1000000;
    if (milliseconds <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static int transfer(int fd, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;

    while (bytes) {
        struct pollfd item = { .fd = fd,
                               .events = writing ? POLLOUT : POLLIN };
        int timeout = remaining_ms();
        ssize_t count;
        int ready;

        if (timeout < 0)
            return -1;
        ready = poll(&item, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            if (!ready)
                errno = ETIMEDOUT;
            return -1;
        }
        count = writing ? write(fd, cursor, bytes) : read(fd, cursor, bytes);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (!count)
                errno = EPIPE;
            return -1;
        }
        cursor += count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int map_view(struct view *view, int fd, size_t bytes, int protection)
{
    void *middle;

    view->reservation = mmap(NULL, bytes * 3, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (view->reservation == MAP_FAILED) {
        view->reservation = NULL;
        return -1;
    }
    view->bytes = bytes;
    middle = (char *)view->reservation + bytes;
    /* MAP_FIXED replaces only the middle of our own reserved range. */
    if (mmap(middle, bytes, protection, MAP_SHARED | MAP_FIXED, fd, 0)
        == MAP_FAILED) {
        int saved = errno;
        munmap(view->reservation, bytes * 3);
        view->reservation = NULL;
        errno = saved;
        return -1;
    }
    view->data = middle;
    return 0;
}

static int unmap_view(struct view *view)
{
    if (view->reservation && munmap(view->reservation, view->bytes * 3) < 0)
        return -1;
    view->reservation = NULL;
    view->data = NULL;
    return 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void reader(int fd, int command, int response, struct view inherited)
{
    struct view ro = {0};
    struct reply reply = {0};
    unsigned char token;

    /* fork inherited RW access; remove that alias before creating the RO view. */
    if (unmap_view(&inherited) < 0 ||
        map_view(&ro, fd, inherited.bytes, PROT_READ) < 0 || close(fd) < 0)
        _exit(10);
    reply.address = (uintptr_t)ro.data;
    if (transfer(response, &reply, sizeof(reply), 1) < 0)
        goto fail;
    for (int round = 0; round < 2; ++round) {
        if (transfer(command, &token, 1, 0) < 0 || token != 'R')
            goto fail;
        reply.first = ro.data[0];
        reply.last = ro.data[ro.bytes / sizeof(uint32_t) - 1];
        if (transfer(response, &reply, sizeof(reply), 1) < 0)
            goto fail;
    }
    if (transfer(command, &token, 1, 0) < 0 || token != 'W')
        goto fail;
    ro.data[0] = 999;
    /* Reaching here means the expected protection fault did not happen. */
fail:
    unmap_view(&ro);
    close(command);
    close(response);
    _exit(11);
}

static int wait_child(pid_t child, int *status)
{
    for (;;) {
        pid_t got = waitpid(child, status, WNOHANG);
        int timeout;

        if (got == child)
            return 0;
        if (got < 0 && errno != EINTR)
            return -1;
        timeout = remaining_ms();
        if (timeout < 0)
            return -1;
        /* Bounded exit observation only; protocol ordering uses pipe tokens. */
        if (poll(NULL, 0, timeout < 10 ? timeout : 10) < 0 && errno != EINTR)
            return -1;
    }
}

int main(int argc, char **argv)
{
    struct view rw = {0};
    struct reply reply;
    struct rlimit core_limit = {0, 0};
    struct sigaction action = {0};
    int commands[2] = {-1, -1}, responses[2] = {-1, -1};
    int fd = -1, status = 0, result = 1, reaped = 0, unmapped = 0;
    int abort_after_map = argc == 2 && !strcmp(argv[1], "--abort-after-map");
    pid_t child = -1;
    long page_bytes;
    unsigned char token;

    if (argc != 1 && !abort_after_map) {
        fprintf(stderr, "usage: %s [--abort-after-map]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
        goto failure;
    deadline.tv_sec += 5;
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        setrlimit(RLIMIT_CORE, &core_limit) < 0 ||
        prctl(PR_SET_DUMPABLE, 0) < 0)
        goto failure;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0)
        goto failure;
    action.sa_handler = SIG_DFL;
    if (sigaction(SIGSEGV, &action, NULL) < 0 ||
        sigaction(SIGCHLD, &action, NULL) < 0)
        goto failure;
    page_bytes = sysconf(_SC_PAGESIZE);
    if (page_bytes < (long)sizeof(uint32_t) ||
        (uintmax_t)page_bytes > SIZE_MAX / 3) {
        errno = EINVAL;
        goto failure;
    }
    fd = memfd_create("mapping-model", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)page_bytes) < 0 ||
        map_view(&rw, fd, (size_t)page_bytes, PROT_READ | PROT_WRITE) < 0 ||
        pipe(commands) < 0 || pipe(responses) < 0)
        goto failure;
    child = fork();
    if (child < 0)
        goto failure;
    if (!child) {
        close_fd(&commands[1]);
        close_fd(&responses[0]);
        reader(fd, commands[0], responses[1], rw);
    }
    close_fd(&commands[0]);
    close_fd(&responses[1]);
    if (transfer(responses[0], &reply, sizeof(reply), 0) < 0)
        goto failure;
    printf("event=mapped bytes=%zu parent_address=0x%" PRIxPTR
           " child_address=0x%" PRIxPTR " parent_prot=rw child_prot=r guards=none\n",
           rw.bytes, (uintptr_t)rw.data, reply.address);
    if (close(fd) < 0) {
        fd = -1;
        goto failure;
    }
    fd = -1;
    puts("event=fd_closed parent=1 child=1");
    if (abort_after_map) {
        fprintf(stderr, "requested abort after map\n");
        goto cleanup;
    }
    for (int round = 1; round <= 2; ++round) {
        uint32_t first = round == 1 ? 7 : 42;
        uint32_t last = first * 10;

        rw.data[0] = first;
        rw.data[rw.bytes / sizeof(uint32_t) - 1] = last;
        printf("event=published round=%d first=%u last=%u\n", round, first, last);
        token = 'R';
        if (transfer(commands[1], &token, 1, 1) < 0 ||
            transfer(responses[0], &reply, sizeof(reply), 0) < 0)
            goto failure;
        if (reply.first != first || reply.last != last) {
            errno = EIO;
            goto failure;
        }
        printf("event=read_ack round=%d first=%u last=%u\n",
               round, reply.first, reply.last);
    }
    token = 'W';
    if (transfer(commands[1], &token, 1, 1) < 0 || wait_child(child, &status) < 0)
        goto failure;
    child = -1;
    ++reaped;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        errno = EPROTO;
        goto failure;
    }
    printf("event=fault kind=ro_write signal=%d\n", WTERMSIG(status));
    if (rw.data[0] != 42 || rw.data[rw.bytes / sizeof(uint32_t) - 1] != 420) {
        errno = EIO;
        goto failure;
    }
    puts("event=preserved first=42 last=420");
    child = fork();
    if (child < 0)
        goto failure;
    if (!child) {
        volatile uint32_t value = rw.data[rw.bytes / sizeof(uint32_t)];
        (void)value;
        _exit(12);
    }
    if (wait_child(child, &status) < 0)
        goto failure;
    child = -1;
    ++reaped;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        errno = EPROTO;
        goto failure;
    }
    printf("event=fault kind=guard_read signal=%d\n", WTERMSIG(status));
    result = 0;
    goto cleanup;

failure:
    perror("mapping experiment");
cleanup:
    close_fd(&fd);
    close_fd(&commands[0]);
    close_fd(&commands[1]);
    close_fd(&responses[0]);
    close_fd(&responses[1]);
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) {
                perror("cleanup waitpid");
                child = -1;
                break;
            }
        }
        if (child > 0)
            ++reaped;
    }
    if (unmap_view(&rw) < 0) {
        perror("munmap");
        result = 1;
    } else {
        unmapped = 1;
    }
    printf("event=cleanup children_reaped=%d unmapped=%d result=%s\n",
           reaped, unmapped, result ? "failed" : "ok");
    return result;
}
