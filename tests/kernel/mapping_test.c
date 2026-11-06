#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mini_binder.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "mapping_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)

struct peer { pid_t pid; int command; int report; };
struct operation { char kind; unsigned int index, size, seed; };
struct report {
    struct mb_buffer_request request;
    struct mb_buffer_stats stats;
};
struct call_result { int error; struct mb_buffer_call call; };

static struct peer children[64];
static unsigned int child_count;
static pid_t owner;
static int observer = -1;
static uint64_t expected_copies, expected_bytes;

static void cleanup(void)
{
    if (getpid() != owner)
        return;
    for (unsigned int i = 0; i < child_count; ++i)
        if (children[i].pid > 0)
            kill(children[i].pid, SIGKILL);
    for (unsigned int i = 0; i < child_count; ++i)
        if (children[i].pid > 0) {
            int status;
            while (waitpid(children[i].pid, &status, 0) < 0 && errno == EINTR)
                ;
        }
}

static void timeout_handler(int number)
{
    (void)number;
    for (unsigned int i = 0; i < child_count; ++i)
        if (children[i].pid > 0)
            kill(children[i].pid, SIGKILL);
    _exit(124);
}

static void track(struct peer peer)
{
    for (unsigned int i = 0; i < child_count; ++i)
        if (!children[i].pid) {
            children[i] = peer;
            return;
        }
    CHECK(child_count < sizeof(children) / sizeof(children[0]));
    children[child_count++] = peer;
}

static void transfer(int fd, void *data, size_t size, int writing)
{
    unsigned char *cursor = data;
    while (size) {
        struct pollfd item = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        ssize_t count;
        int ready = poll(&item, 1, 5000);
        if (ready < 0 && errno == EINTR)
            continue;
        CHECK(ready == 1);
        count = writing ? write(fd, cursor, size) : read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        cursor += count;
        size -= (size_t)count;
    }
}

static int open_device(void)
{
    int fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static void reap_status(struct peer peer, int expected_signal)
{
    int status;
    pid_t got;
    do {
        got = waitpid(peer.pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    CHECK(got == peer.pid);
    for (unsigned int i = 0; i < child_count; ++i)
        if (children[i].pid == peer.pid)
            children[i].pid = 0;
    if (peer.command >= 0)
        close(peer.command);
    if (peer.report >= 0)
        close(peer.report);
    if (expected_signal)
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == expected_signal);
    else
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void deadline_start(struct timespec *start)
{
    CHECK(clock_gettime(CLOCK_MONOTONIC, start) == 0);
}

static void deadline_check(struct timespec start)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    CHECK(now.tv_sec - start.tv_sec < 5);
    sched_yield();
}

static void await_counts(unsigned int queued, unsigned int delivered)
{
    struct timespec start;
    struct mb_stats stats;
    deadline_start(&start);
    for (;;) {
        CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0);
        if (stats.queued == queued && stats.delivered == delivered &&
            stats.live_requests == queued + delivered)
            return;
        deadline_check(start);
    }
}

static void await_contexts(uint64_t count)
{
    struct timespec start;
    struct mb_query query;
    deadline_start(&start);
    for (;;) {
        CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0);
        if (query.live_contexts == count)
            return;
        deadline_check(start);
    }
}

static void await_regions_clean(void)
{
    struct timespec start;
    struct mb_buffer_stats stats;
    deadline_start(&start);
    for (;;) {
        CHECK(ioctl(observer, MB_IOC_BUFFER_STATS, &stats) == 0);
        if (!stats.live_regions && !stats.live_slots)
            return;
        deadline_check(start);
    }
}

static unsigned char payload_byte(unsigned int seed, unsigned int index)
{
    return (unsigned char)(seed + index * 17U);
}

static uint32_t checksum(unsigned int seed, unsigned int size)
{
    uint32_t sum = 0;
    for (unsigned int i = 0; i < size; ++i)
        sum += payload_byte(seed, i);
    return sum;
}

static void rejected_map(int fd, size_t bytes, int protection, int flags, off_t offset)
{
    void *mapping = mmap(NULL, bytes, protection, flags, fd, offset);
    CHECK(mapping == MAP_FAILED);
}

static void mapping_permissions(void)
{
    int fd = open_device();
    unsigned char *base;
    struct mb_buffer_stats stats;
    struct peer probe = { .command = -1, .report = -1 };

    CHECK(ioctl(fd, MB_IOC_REGISTER_BUFFER) == -1 && errno == ENXIO);
    probe.pid = fork();
    CHECK(probe.pid >= 0);
    if (!probe.pid) {
        void *attempt = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
        CHECK(attempt == MAP_FAILED && errno == EPERM);
        _exit(0);
    }
    track(probe);
    reap_status(probe, 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_READ, MAP_PRIVATE, 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_READ, MAP_SHARED, 4096);
    rejected_map(fd, MB_MAP_BYTES - 4096, PROT_READ, MAP_SHARED, 0);
    rejected_map(fd, MB_MAP_BYTES + 4096, PROT_READ, MAP_SHARED, 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_NONE, MAP_SHARED, 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_READ | PROT_EXEC, MAP_SHARED, 0);
    base = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(base != MAP_FAILED);
    for (unsigned int i = 0; i < MB_MAP_BYTES; ++i)
        CHECK(base[i] == 0);
    CHECK(mprotect(base, MB_MAP_BYTES, PROT_READ | PROT_WRITE) == -1);
    CHECK(mprotect(base, MB_MAP_BYTES, PROT_READ | PROT_EXEC) == -1);
    CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &stats) == 0);
    CHECK(stats.mapped_vmas == 1 && stats.used_slots == 0);

    probe.pid = fork();
    CHECK(probe.pid >= 0);
    if (!probe.pid) {
        unsigned char residency;
        struct mb_query query;
        CHECK(mincore(base, 4096, &residency) == -1 && errno == ENOMEM);
        CHECK(ioctl(fd, MB_IOC_QUERY, &query) == -1 && errno == EPERM);
        _exit(0);
    }
    track(probe);
    reap_status(probe, 0);

    CHECK(munmap(base + 4096, 4096) == 0);
    CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &stats) == 0 && stats.mapped_vmas == 2);
    CHECK(munmap(base, 4096) == 0);
    CHECK(munmap(base + 8192, MB_MAP_BYTES - 8192) == 0);
    CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &stats) == 0 && stats.mapped_vmas == 0);
    rejected_map(fd, MB_MAP_BYTES, PROT_READ, MAP_SHARED, 0);
    CHECK(close(fd) == 0);

    probe.pid = fork();
    CHECK(probe.pid >= 0);
    if (!probe.pid) {
        int own = open_device();
        volatile unsigned char *ro = mmap(NULL, MB_MAP_BYTES, PROT_READ,
                                         MAP_SHARED, own, 0);
        CHECK(ro != MAP_FAILED);
        CHECK(ro[0] == 0);
        CHECK(close(own) == 0);
        CHECK(ro[0] == 0);
        ro[0] = 1;
        _exit(2);
    }
    track(probe);
    reap_status(probe, SIGSEGV);
    await_contexts(1);
    await_regions_clean();
    puts("MAPPING_STAGE permissions=ok split_vmas=ok dontcopy=ok");
}

static struct report server_command(struct peer server, char kind,
                                    unsigned int index, unsigned int size,
                                    unsigned int seed)
{
    struct operation operation = { kind, index, size, seed };
    struct report report;
    transfer(server.command, &operation, sizeof(operation), 1);
    transfer(server.report, &report, sizeof(report), 0);
    return report;
}

static void stop_server(struct peer server)
{
    struct operation operation = { .kind = 'X' };
    transfer(server.command, &operation, sizeof(operation), 1);
    reap_status(server, 0);
}

static void server_loop(int commands, int reports, int legacy)
{
    int fd = open_device();
    unsigned char *base = MAP_FAILED;
    struct mb_buffer_request saved[MB_MAP_SLOTS] = {{0}};
    uint32_t sums[MB_MAP_SLOTS] = {0};
    struct report report = {0};

    if (legacy) {
        CHECK(ioctl(fd, MB_IOC_REGISTER) == 0);
    } else {
        base = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
        CHECK(base != MAP_FAILED);
        CHECK(ioctl(fd, MB_IOC_REGISTER_BUFFER) == 0);
        CHECK(ioctl(fd, MB_IOC_REGISTER_BUFFER) == -1 && errno == EBUSY);
    }
    transfer(reports, &report, sizeof(report), 1);
    for (;;) {
        struct operation operation;
        struct mb_reply reply = {0};
        struct mb_buffer_request *request;
        transfer(commands, &operation, sizeof(operation), 0);
        CHECK(operation.index < MB_MAP_SLOTS);
        request = &saved[operation.index];
        memset(&report, 0, sizeof(report));
        if (operation.kind == 'X')
            break;
        CHECK(!legacy);
        switch (operation.kind) {
        case 'R':
            CHECK(ioctl(fd, MB_IOC_RECEIVE_BUFFER, request) == 0);
            CHECK(request->reserved == 0 && request->token != 0);
            CHECK(request->size == operation.size);
            CHECK(request->offset % MB_MAX_PAYLOAD == 0);
            CHECK(request->offset <= MB_MAP_BYTES - MB_MAX_PAYLOAD);
            CHECK(base != MAP_FAILED);
            for (unsigned int i = 0; i < request->size; ++i)
                CHECK(base[request->offset + i] == payload_byte(operation.seed, i));
            for (unsigned int i = request->size; i < MB_MAX_PAYLOAD; ++i)
                CHECK(base[request->offset + i] == 0);
            sums[operation.index] = checksum(operation.seed, operation.size);
            report.request = *request;
            break;
        case 'F':
            CHECK(ioctl(fd, MB_IOC_RECEIVE_BUFFER, (void *)1) == -1 && errno == EFAULT);
            break;
        case 'P':
        case 'L':
            reply.id = request->id;
            reply.result = sums[operation.index];
            if (operation.kind == 'L')
                CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == -1 && errno == ENOENT);
            else
                CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == 0);
            break;
        case 'D':
            CHECK(ioctl(fd, MB_IOC_FREE_BUFFER, &request->token) == 0);
            CHECK(ioctl(fd, MB_IOC_FREE_BUFFER, &request->token) == -1 && errno == ENOENT);
            break;
        case 'M':
            CHECK(base != MAP_FAILED && munmap(base, MB_MAP_BYTES) == 0);
            base = MAP_FAILED;
            break;
        case 'C':
            CHECK(close(fd) == 0);
            fd = -1;
            CHECK(base != MAP_FAILED && base[0] == 0);
            break;
        case 'B':
            break;
        default:
            CHECK(0);
        }
        if (fd >= 0)
            CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &report.stats) == 0);
        transfer(reports, &report, sizeof(report), 1);
    }
    if (base != MAP_FAILED)
        CHECK(munmap(base, MB_MAP_BYTES) == 0);
    if (fd >= 0)
        CHECK(close(fd) == 0);
}

static struct peer start_server(int legacy)
{
    int commands[2], reports[2];
    struct peer peer;
    struct report ready;
    CHECK(pipe(commands) == 0 && pipe(reports) == 0);
    peer.pid = fork();
    CHECK(peer.pid >= 0);
    if (!peer.pid) {
        alarm(30);
        close(observer);
        close(commands[1]);
        close(reports[0]);
        server_loop(commands[0], reports[1], legacy);
        _exit(0);
    }
    close(commands[0]);
    close(reports[1]);
    peer.command = commands[1];
    peer.report = reports[0];
    track(peer);
    transfer(peer.report, &ready, sizeof(ready), 0);
    expected_copies = expected_bytes = 0;
    return peer;
}

static void interrupt_handler(int number) { (void)number; }

static struct peer start_client(unsigned int seed, unsigned int size)
{
    int reports[2];
    struct peer peer = { .command = -1 };
    CHECK(pipe(reports) == 0);
    peer.pid = fork();
    CHECK(peer.pid >= 0);
    if (!peer.pid) {
        unsigned char data[MB_MAX_PAYLOAD];
        int fd;
        struct sigaction action = { .sa_handler = interrupt_handler };
        struct call_result result = {0};
        alarm(30);
        close(observer);
        close(reports[0]);
        sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        fd = open_device();
        for (unsigned int i = 0; i < size; ++i)
            data[i] = payload_byte(seed, i);
        result.call.data = (uintptr_t)data;
        result.call.size = size;
        if (ioctl(fd, MB_IOC_CALL_BUFFER, &result.call) < 0)
            result.error = errno;
        CHECK(close(fd) == 0);
        transfer(reports[1], &result, sizeof(result), 1);
        _exit(0);
    }
    close(reports[1]);
    peer.report = reports[0];
    track(peer);
    return peer;
}

static struct call_result client_result(struct peer client)
{
    struct call_result result;
    transfer(client.report, &result, sizeof(result), 0);
    reap_status(client, 0);
    return result;
}

static void account(unsigned int size)
{
    ++expected_copies;
    expected_bytes += size;
}

static void assert_stats(struct report report, unsigned int slots, unsigned int vmas)
{
    CHECK(report.stats.used_slots == slots && report.stats.mapped_vmas == vmas);
    CHECK(report.stats.payload_copies == expected_copies);
    CHECK(report.stats.payload_bytes == expected_bytes);
}

static void assert_registered(int registered)
{
    struct mb_stats stats;
    CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0);
    CHECK(stats.server_registered == (unsigned int)registered);
}

static void invalid_calls(struct peer server)
{
    unsigned char data = 9;
    unsigned char *partial;
    struct mb_buffer_call call = { .data = (uintptr_t)&data, .size = 1 };
    struct mb_call old_call = {0};
    uint64_t token = 0;

    CHECK(ioctl(observer, MB_IOC_CALL, &old_call) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, (void *)1) == -1 && errno == EFAULT);
    call.size = 0;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EINVAL);
    call.size = MB_MAX_PAYLOAD + 1;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EINVAL);
    call.size = 1;
    call.reserved = 1;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EINVAL);
    call.reserved = 0;
    call.data = 1;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EFAULT);
    call.data = 0;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EFAULT);
    partial = mmap(NULL, MB_MAX_PAYLOAD * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(partial != MAP_FAILED);
    memset(partial, 0x5a, MB_MAX_PAYLOAD);
    CHECK(mprotect(partial + MB_MAX_PAYLOAD, MB_MAX_PAYLOAD, PROT_NONE) == 0);
    call.data = (uintptr_t)(partial + MB_MAX_PAYLOAD - 2);
    call.size = 4;
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EFAULT);
    CHECK(munmap(partial, MB_MAX_PAYLOAD * 2) == 0);
    CHECK(ioctl(observer, MB_IOC_FREE_BUFFER, &token) == -1 && errno == ENOENT);
    assert_stats(server_command(server, 'B', 0, 0, 0), 0, 1);
    await_counts(0, 0);
}

static void payload_and_capacity(struct peer server)
{
    const unsigned int sizes[] = {1, 7, 511, 4096, 3};
    struct peer client;
    struct call_result value;
    struct report report;
    uint64_t tokens[MB_MAP_SLOTS], offsets[MB_MAP_SLOTS];

    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        client = start_client(11 + i, sizes[i]);
        await_counts(1, 0);
        account(sizes[i]);
        if (!i) {
            assert_stats(server_command(server, 'F', 0, 0, 0), 1, 1);
            await_counts(1, 0);
        }
        report = server_command(server, 'R', 0, sizes[i], 11 + i);
        assert_stats(report, 1, 1);
        CHECK(ioctl(observer, MB_IOC_FREE_BUFFER, &report.request.token) == -1 &&
              errno == ENOENT);
        assert_stats(server_command(server, 'P', 0, 0, 0), 1, 1);
        value = client_result(client);
        CHECK(value.error == 0 && value.call.status == 0 &&
              value.call.result == checksum(11 + i, sizes[i]));
        await_counts(0, 0);
        assert_stats(server_command(server, 'D', 0, 0, 0), 0, 1);
    }
    for (unsigned int i = 0; i < MB_MAP_SLOTS; ++i) {
        client = start_client(40 + i, 23 + i);
        await_counts(1, 0);
        account(23 + i);
        report = server_command(server, 'R', i, 23 + i, 40 + i);
        tokens[i] = report.request.token;
        offsets[i] = report.request.offset;
        for (unsigned int j = 0; j < i; ++j)
            CHECK(tokens[i] != tokens[j] && offsets[i] != offsets[j]);
        assert_stats(report, i + 1, 1);
        assert_stats(server_command(server, 'P', i, 0, 0), i + 1, 1);
        value = client_result(client);
        CHECK(value.error == 0 && value.call.result == checksum(40 + i, 23 + i));
        await_counts(0, 0);
    }
    client = start_client(90, 5);
    value = client_result(client);
    CHECK(value.error == EAGAIN);
    assert_stats(server_command(server, 'B', 0, 0, 0), MB_MAP_SLOTS, 1);
    assert_stats(server_command(server, 'D', 0, 0, 0), MB_MAP_SLOTS - 1, 1);
    client = start_client(91, 3);
    await_counts(1, 0);
    account(3);
    report = server_command(server, 'R', 0, 3, 91);
    CHECK(report.request.token != tokens[0]);
    assert_stats(report, MB_MAP_SLOTS, 1);
    server_command(server, 'P', 0, 0, 0);
    value = client_result(client);
    CHECK(value.error == 0 && value.call.result == checksum(91, 3));
    await_counts(0, 0);
    for (unsigned int i = 0; i < MB_MAP_SLOTS; ++i)
        assert_stats(server_command(server, 'D', i, 0, 0), MB_MAP_SLOTS - i - 1, 1);
    puts("MAPPING_STAGE payloads=5 receive_rollback=ok slots=16 free_recovery=ok");
}

static void cancellation(struct peer server)
{
    struct peer client;
    struct call_result value;
    for (int delivered = 0; delivered < 2; ++delivered) {
        client = start_client(101, 33);
        await_counts(1, 0);
        account(33);
        if (delivered) {
            assert_stats(server_command(server, 'R', 0, 33, 101), 1, 1);
            await_counts(0, 1);
        }
        CHECK(kill(client.pid, SIGUSR1) == 0);
        value = client_result(client);
        CHECK(value.error == EINTR);
        await_counts(0, 0);
        assert_stats(server_command(server, 'B', 0, 0, 0), delivered ? 1 : 0, 1);
        if (delivered) {
            assert_stats(server_command(server, 'L', 0, 0, 0), 1, 1);
            assert_stats(server_command(server, 'D', 0, 0, 0), 0, 1);
        }
    }
    puts("MAPPING_STAGE cancellation=queued_auto_free,delivered_explicit_free");
}

static void lifetime_cases(void)
{
    struct peer server, client;
    struct call_result value;

    server = start_server(0);
    client = start_client(121, 19);
    await_counts(1, 0);
    account(19);
    assert_stats(server_command(server, 'R', 0, 19, 121), 1, 1);
    assert_stats(server_command(server, 'M', 0, 0, 0), 1, 0);
    assert_registered(1);
    assert_stats(server_command(server, 'D', 0, 0, 0), 0, 0);
    assert_stats(server_command(server, 'P', 0, 0, 0), 0, 0);
    value = client_result(client);
    CHECK(value.error == 0 && value.call.result == checksum(121, 19));
    await_counts(0, 0);
    stop_server(server);
    await_contexts(1);

    for (int delivered = 0; delivered < 2; ++delivered) {
        server = start_server(0);
        client = start_client(131, 29);
        await_counts(1, 0);
        account(29);
        if (delivered) {
            assert_stats(server_command(server, 'R', 0, 29, 131), 1, 1);
            await_counts(0, 1);
        }
        stop_server(server);
        value = client_result(client);
        CHECK(value.error == EPIPE);
        await_counts(0, 0);
        assert_registered(0);
        await_contexts(1);
    }

    server = start_server(0);
    server_command(server, 'C', 0, 0, 0);
    assert_registered(1);
    client = start_client(141, 31);
    await_counts(1, 0);
    stop_server(server);
    value = client_result(client);
    CHECK(value.error == EPIPE);
    await_counts(0, 0);
    assert_registered(0);
    await_contexts(1);
    puts("MAPPING_STAGE lifetime=munmap_free,server_exit,close_with_vma contexts=1");
}

int main(int argc, char **argv)
{
    struct rlimit no_core = {0, 0};
    struct sigaction action = { .sa_handler = timeout_handler };
    struct mb_query query;
    struct peer server;
    unsigned char data = 1;
    struct mb_buffer_call call = { .data = (uintptr_t)&data, .size = 1 };

    owner = getpid();
    CHECK(atexit(cleanup) == 0);
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    action.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0);
    action.sa_handler = SIG_DFL;
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
    CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0 && prctl(PR_SET_DUMPABLE, 0) == 0);
    alarm(30);
    observer = open_device();
    CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0);
    CHECK(query.version == MB_ABI_VERSION && (query.features & MB_FEATURE_MMAP));
    CHECK(query.live_contexts == 1);
    if (argc == 2 && !strcmp(argv[1], "--allocation-failure")) {
        struct mb_buffer_stats stats;

        for (unsigned int attempt = 0; attempt < 2; ++attempt) {
            CHECK(mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED,
                       observer, 0) == MAP_FAILED && errno == ENOMEM);
            CHECK(ioctl(observer, MB_IOC_BUFFER_STATS, &stats) == 0);
            CHECK(stats.used_slots == 0 && stats.mapped_vmas == 0 &&
                  stats.live_regions == 0 && stats.live_slots == 0);
        }
        CHECK(close(observer) == 0);
        observer = -1;
        puts("MAPPING_ALLOCATION_FAILURE_PASS attempts=2 regions=0 slots=0");
        return 0;
    }
    CHECK(argc == 1);
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == ENOTCONN);
    mapping_permissions();

    server = start_server(1);
    CHECK(ioctl(observer, MB_IOC_CALL_BUFFER, &call) == -1 && errno == EPROTOTYPE);
    stop_server(server);
    await_contexts(1);
    server = start_server(0);
    invalid_calls(server);
    payload_and_capacity(server);
    cancellation(server);
    stop_server(server);
    await_contexts(1);
    lifetime_cases();
    await_regions_clean();
    CHECK(close(observer) == 0);
    observer = -1;
    puts("MAPPING_TEST_PASS slots=16 payload_copies=validated requests=0 contexts=clean");
    return 0;
}
