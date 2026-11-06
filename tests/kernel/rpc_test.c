#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mini_binder.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "rpc_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)

struct peer { pid_t pid; int command; int report; };
struct call_result { int error; struct mb_call call; };
static int observer;

static void transfer(int fd, void *data, size_t size, int writing)
{
    char *cursor = data;
    while (size) {
        ssize_t count = writing ? write(fd, cursor, size) : read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        size -= (size_t)count;
        cursor += count;
    }
}

static int open_device(void)
{
    int fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static void reap(struct peer peer)
{
    int status;
    CHECK(waitpid(peer.pid, &status, 0) == peer.pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(peer.command);
    close(peer.report);
}

static void command(struct peer peer, char value)
{
    transfer(peer.command, &value, 1, 1);
}

static void server_loop(int commands, int reports)
{
    int fd = open_device();
    char value = 'S';
    struct mb_request request = { 0 };
    struct mb_reply reply;

    CHECK(ioctl(fd, MB_IOC_REGISTER) == 0);
    transfer(reports, &value, 1, 1);
    for (;;) {
        transfer(commands, &value, 1, 0);
        if (value == 'X')
            break;
        if (value == 'B') {
            struct mb_call call = { 1, 2, 0, 0 };
            CHECK(ioctl(fd, MB_IOC_REGISTER) == -1 && errno == EBUSY);
            CHECK(ioctl(fd, MB_IOC_CALL, &call) == -1 && errno == EDEADLK);
            CHECK(ioctl(fd, MB_IOC_REPLY, (void *)1) == -1 && errno == EFAULT);
            reply.id = 0;
            reply.result = 0;
            reply.status = 0;
            CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == -1 && errno == ENOENT);
            transfer(reports, &value, 1, 1);
            continue;
        }
        if (value == 'R' || value == 'H' || value == 'F') {
            if (value == 'F')
                CHECK(ioctl(fd, MB_IOC_RECEIVE, (void *)1) == -1 && errno == EFAULT);
            CHECK(ioctl(fd, MB_IOC_RECEIVE, &request) == 0);
            transfer(reports, &request, sizeof(request), 1);
            if (value != 'R')
                continue;
        }
        CHECK(value == 'R' || value == 'P' || value == 'L');
        reply.id = request.id;
        reply.status = request.a > UINT32_MAX - request.b ? -EOVERFLOW : 0;
        reply.result = reply.status ? 0 : request.a + request.b;
        if (value == 'L') {
            CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == -1 && errno == ENOENT);
        } else {
            CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == 0);
            CHECK(ioctl(fd, MB_IOC_REPLY, &reply) == -1 && errno == ENOENT);
        }
        if (value != 'R')
            transfer(reports, &value, 1, 1);
    }
    CHECK(close(fd) == 0);
}

static struct peer start_server(void)
{
    int commands[2], reports[2];
    struct peer peer;
    char ready;

    CHECK(pipe(commands) == 0 && pipe(reports) == 0);
    peer.pid = fork();
    CHECK(peer.pid >= 0);
    if (!peer.pid) {
        close(observer);
        close(commands[1]);
        close(reports[0]);
        server_loop(commands[0], reports[1]);
        _exit(0);
    }
    close(commands[0]);
    close(reports[1]);
    peer.command = commands[1];
    peer.report = reports[0];
    transfer(peer.report, &ready, 1, 0);
    CHECK(ready == 'S');
    return peer;
}

static void signal_handler(int number) { (void)number; }

static struct peer start_client_options(uint32_t a, uint32_t b, int flags, int readonly)
{
    int reports[2];
    struct peer peer = { .command = -1 };
    CHECK(pipe(reports) == 0);
    peer.pid = fork();
    CHECK(peer.pid >= 0);
    if (!peer.pid) {
        struct sigaction action = { .sa_handler = signal_handler };
        struct call_result result = { .call = { a, b, 0, 0 } };
        struct mb_call *input = &result.call;
        int fd;
        close(observer);
        close(reports[0]);
        sigemptyset(&action.sa_mask);
        action.sa_flags = flags;
        CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
        fd = open_device();
        if (readonly) {
            input = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            CHECK(input != MAP_FAILED);
            *input = result.call;
            CHECK(mprotect(input, 4096, PROT_READ) == 0);
        }
        if (ioctl(fd, MB_IOC_CALL, input) < 0)
            result.error = errno;
        if (readonly)
            CHECK(munmap(input, 4096) == 0);
        CHECK(close(fd) == 0);
        transfer(reports[1], &result, sizeof(result), 1);
        _exit(0);
    }
    close(reports[1]);
    peer.report = reports[0];
    return peer;
}

static struct peer start_client(uint32_t a, uint32_t b)
{
    return start_client_options(a, b, 0, 0);
}

static void *thread_call(void *argument)
{
    struct call_result *output = argument;
    if (ioctl(observer, MB_IOC_CALL, &output->call) < 0)
        output->error = errno;
    return NULL;
}

static struct call_result result(struct peer client)
{
    struct call_result value;
    struct pollfd report = { .fd = client.report, .events = POLLIN };
    CHECK(poll(&report, 1, 5000) == 1 && (report.revents & POLLIN));
    transfer(client.report, &value, sizeof(value), 0);
    reap(client);
    return value;
}

static void await_counts(unsigned int queued, unsigned int delivered)
{
    struct timespec start, now;
    struct mb_stats stats;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0);
        if (stats.queued == queued && stats.delivered == delivered &&
            stats.live_requests == queued + delivered)
            return;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

int main(void)
{
    struct peer server, client, clients[MB_MAX_REQUESTS];
    struct mb_call call = { 1, 2, 0, 0 };
    struct mb_reply fake = { 0 };
    struct mb_request request;
    struct call_result value;
    char ack;

    alarm(30);
    observer = open_device();
    CHECK(ioctl(observer, MB_IOC_CALL, &call) == -1 && errno == ENOTCONN);
    CHECK(ioctl(observer, MB_IOC_CALL, (void *)1) == -1 && errno == EFAULT);
    server = start_server();
    CHECK(ioctl(observer, MB_IOC_REGISTER) == -1 && errno == EBUSY);
    CHECK(ioctl(observer, MB_IOC_RECEIVE, &request) == -1 && errno == EPERM);
    CHECK(ioctl(observer, MB_IOC_REPLY, &fake) == -1 && errno == EPERM);
    command(server, 'B');
    transfer(server.report, &ack, 1, 0);
    CHECK(ack == 'B');

    for (unsigned int i = 0; i < 8; ++i)
        clients[i] = start_client(i, 100);
    await_counts(8, 0);
    for (unsigned int i = 0; i < 8; ++i) {
        command(server, 'R');
        transfer(server.report, &request, sizeof(request), 0);
        CHECK(request.b == 100);
    }
    for (unsigned int i = 0; i < 8; ++i) {
        value = result(clients[i]);
        CHECK(value.error == 0 && value.call.status == 0 && value.call.result == i + 100);
    }
    await_counts(0, 0);

    {
        pthread_t worker;
        struct call_result output = { .call = { 1, 2, 0, 0 } };
        CHECK(pthread_create(&worker, NULL, thread_call, &output) == 0);
        await_counts(1, 0);
        CHECK(ioctl(observer, MB_IOC_CALL, &call) == -1 && errno == EBUSY);
        command(server, 'R');
        transfer(server.report, &request, sizeof(request), 0);
        CHECK(pthread_join(worker, NULL) == 0);
        CHECK(output.error == 0 && output.call.result == 3);
        await_counts(0, 0);
    }

    client = start_client_options(1, 2, 0, 1);
    await_counts(1, 0);
    command(server, 'R');
    transfer(server.report, &request, sizeof(request), 0);
    value = result(client);
    CHECK(value.error == EFAULT);
    await_counts(0, 0);

    for (unsigned int i = 0; i < MB_MAX_REQUESTS; ++i)
        clients[i] = start_client(i, 1);
    await_counts(MB_MAX_REQUESTS, 0);
    client = start_client(1, 2);
    value = result(client);
    CHECK(value.error == EAGAIN);
    for (unsigned int i = 0; i < MB_MAX_REQUESTS; ++i)
        CHECK(kill(clients[i].pid, SIGUSR1) == 0);
    for (unsigned int i = 0; i < MB_MAX_REQUESTS; ++i) {
        value = result(clients[i]);
        CHECK(value.error == EINTR);
    }
    await_counts(0, 0);

    client = start_client(1, 2);
    await_counts(1, 0);
    CHECK(kill(client.pid, SIGKILL) == 0);
    {
        int status;
        CHECK(waitpid(client.pid, &status, 0) == client.pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        close(client.report);
    }
    await_counts(0, 0);

    client = start_client(UINT32_MAX, 1);
    await_counts(1, 0);
    command(server, 'F');
    transfer(server.report, &request, sizeof(request), 0);
    command(server, 'P');
    transfer(server.report, &ack, 1, 0);
    value = result(client);
    CHECK(value.error == 0 && value.call.status == -EOVERFLOW);
    await_counts(0, 0);

    for (int claimed = 0; claimed < 2; ++claimed) {
        client = start_client(1, 2);
        await_counts(1, 0);
        if (claimed) {
            command(server, 'H');
            transfer(server.report, &request, sizeof(request), 0);
            await_counts(0, 1);
        }
        CHECK(kill(client.pid, SIGUSR1) == 0);
        value = result(client);
        CHECK(value.error == EINTR);
        await_counts(0, 0);
        if (claimed) {
            command(server, 'L');
            transfer(server.report, &ack, 1, 0);
        }
    }
    client = start_client_options(1, 2, SA_RESTART, 0);
    await_counts(1, 0);
    command(server, 'H');
    transfer(server.report, &request, sizeof(request), 0);
    CHECK(kill(client.pid, SIGUSR1) == 0);
    value = result(client);
    CHECK(value.error == EINTR);
    await_counts(0, 0);
    command(server, 'L');
    transfer(server.report, &ack, 1, 0);
    command(server, 'X');
    reap(server);

    for (int claimed = 0; claimed < 2; ++claimed) {
        server = start_server();
        client = start_client(1, 2);
        await_counts(1, 0);
        if (claimed) {
            command(server, 'H');
            transfer(server.report, &request, sizeof(request), 0);
        }
        command(server, 'X');
        reap(server);
        value = result(client);
        CHECK(value.error == EPIPE);
        await_counts(0, 0);
    }
    CHECK(close(observer) == 0);
    puts("RPC_TEST_PASS concurrent=8 capacity=64 errors=validated requests=0");
    return 0;
}
