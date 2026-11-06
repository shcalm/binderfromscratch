#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mini_binder.h"
#include "parcel.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "node_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)

struct fixture {
    int service, client;
    const unsigned char *requests, *replies;
};

struct job {
    pthread_t thread;
    int fd, done[2], result, error, readonly_output;
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_node_call call;
    struct mb_node_call *control;
};

struct object {
    uint64_t id, cookie;
    int kind;
    int32_t value;
};

static int observer = -1;
static pid_t owner;
static volatile sig_atomic_t active_child = -1;

static void cleanup(void)
{
    int status;
    pid_t child = active_child;
    if (getpid() != owner || child <= 0)
        return;
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    active_child = -1;
}

static void deadline(int number)
{
    (void)number;
    if (getpid() == owner && active_child > 0)
        kill(active_child, SIGKILL);
    _exit(124);
}

static void interrupted(int number)
{
    (void)number;
}

static int open_device(void)
{
    int fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static const unsigned char *map_region(int fd)
{
    void *base = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(base != MAP_FAILED);
    return base;
}

static void transfer(int fd, void *buffer, size_t size, int writing)
{
    unsigned char *cursor = buffer;
    while (size) {
        struct pollfd item = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        int ready = poll(&item, 1, 5000);
        ssize_t count;
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

static struct mb_node_stats node_stats(int fd)
{
    struct mb_node_stats stats;
    CHECK(ioctl(fd, MB_IOC_NODE_STATS, &stats) == 0 && !stats.reserved);
    return stats;
}

static struct mb_buffer_stats buffer_stats(int fd)
{
    struct mb_buffer_stats stats;
    CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &stats) == 0);
    return stats;
}

static void refs(int fd, unsigned int owned, unsigned int transactions, unsigned int slots)
{
    struct mb_node_stats stats = node_stats(fd);
    CHECK(stats.live_nodes == owned && stats.owned_nodes == owned);
    CHECK(stats.transaction_refs == transactions && stats.slot_refs == slots);
}

static void await_counts(unsigned int queued, unsigned int delivered)
{
    struct timespec start, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        struct mb_stats stats;
        CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0);
        if (stats.queued == queued && stats.delivered == delivered &&
            stats.live_requests == queued + delivered)
            return;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

static void baseline(void)
{
    struct mb_query query;
    struct mb_stats stats;
    struct mb_buffer_stats buffers;
    await_counts(0, 0);
    CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0 && !stats.server_registered);
    CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0 && query.live_contexts == 1);
    refs(observer, 0, 0, 0);
    buffers = buffer_stats(observer);
    CHECK(!buffers.live_slots && !buffers.live_regions);
}

static void open_fixture(struct fixture *fixture, int legacy)
{
    fixture->service = open_device();
    fixture->client = open_device();
    fixture->requests = map_region(fixture->service);
    fixture->replies = map_region(fixture->client);
    CHECK(ioctl(fixture->service, legacy ? MB_IOC_REGISTER_TRANSACTION : MB_IOC_REGISTER_NODE) == 0);
}

static void close_fixture(struct fixture *fixture)
{
    await_counts(0, 0);
    CHECK(buffer_stats(fixture->service).used_slots == 0);
    CHECK(buffer_stats(fixture->client).used_slots == 0);
    CHECK(munmap((void *)fixture->requests, MB_MAP_BYTES) == 0);
    CHECK(munmap((void *)fixture->replies, MB_MAP_BYTES) == 0);
    CHECK(close(fixture->service) == 0 && close(fixture->client) == 0);
    baseline();
}

static uint64_t publish(int fd, uint64_t cookie)
{
    struct mb_node_publish item = { .cookie = cookie, .id = UINT64_MAX };
    CHECK(ioctl(fd, MB_IOC_PUBLISH_NODE, &item) == 0 && item.id && item.cookie == cookie);
    return item.id;
}

static void remove_node(int fd, uint64_t id)
{
    CHECK(ioctl(fd, MB_IOC_REMOVE_NODE, &id) == 0);
}

static void busy_node(int fd, uint64_t id)
{
    CHECK(ioctl(fd, MB_IOC_REMOVE_NODE, &id) == -1 && errno == EBUSY);
}

static void free_token(int fd, uint64_t token)
{
    CHECK(token && ioctl(fd, MB_IOC_FREE_BUFFER, &token) == 0);
}

static size_t encode(unsigned char *wire, int32_t a, int32_t b)
{
    int32_t values[] = { a, b };
    struct mbp_writer writer;
    size_t size;
    CHECK(mbp_writer_init(&writer, wire, MBP_MAX_BYTES, NULL));
    CHECK(mbp_write_u32(&writer, 77, NULL));
    CHECK(mbp_write_i32(&writer, 0, NULL));
    CHECK(mbp_write_bytes(&writer, NULL, 0, 1, NULL));
    CHECK(mbp_write_array(&writer, values, 2, NULL));
    CHECK(mbp_writer_finish(&writer, &size, NULL));
    return size;
}

static void decode(const unsigned char *wire, size_t size, int32_t values[2])
{
    struct mbp_reader reader;
    uint32_t sequence;
    int32_t status;
    const unsigned char *bytes;
    size_t length, count;
    int is_null;
    CHECK(mbp_reader_init(&reader, wire, size, NULL));
    CHECK(mbp_read_u32(&reader, &sequence, NULL) && sequence == 77);
    CHECK(mbp_read_i32(&reader, &status, NULL) && status == 0);
    CHECK(mbp_read_bytes(&reader, &bytes, &length, &is_null, NULL));
    CHECK(is_null && !bytes && !length);
    CHECK(mbp_read_array(&reader, values, 2, &count, NULL) && count == 2);
    CHECK(mbp_reader_finish(&reader, NULL));
}

static void *call_thread(void *argument)
{
    struct job *job = argument;
    char done = 'D';
    errno = 0;
    job->result = ioctl(job->fd, MB_IOC_CALL_NODE, job->control);
    job->error = errno;
    if (!job->result)
        job->call = *job->control;
    transfer(job->done[1], &done, 1, 1);
    return NULL;
}

static void start_job(struct job *job, int fd, uint64_t target, uint32_t code,
                       int32_t a, int32_t b, int readonly_output)
{
    memset(job, 0, sizeof(*job));
    job->fd = fd;
    job->call.target = target;
    job->call.code = code;
    job->call.call.data = (uintptr_t)job->wire;
    job->call.call.size = (uint32_t)encode(job->wire, a, b);
    job->control = &job->call;
    job->readonly_output = readonly_output;
    if (readonly_output) {
        job->control = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(job->control != MAP_FAILED);
        *job->control = job->call;
        CHECK(mprotect(job->control, 4096, PROT_READ) == 0);
    }
    CHECK(pipe(job->done) == 0);
    CHECK(pthread_create(&job->thread, NULL, call_thread, job) == 0);
}

static void finish_job(struct job *job, int expected_error)
{
    char done;
    struct timespec limit;
    transfer(job->done[0], &done, 1, 0);
    CHECK(done == 'D');
    CHECK(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    CHECK(pthread_timedjoin_np(job->thread, NULL, &limit) == 0);
    CHECK(expected_error ? job->result == -1 && job->error == expected_error : job->result == 0);
    if (job->readonly_output) {
        CHECK(memcmp(job->control, &job->call, sizeof(job->call)) == 0);
        CHECK(munmap(job->control, 4096) == 0);
    }
    CHECK(close(job->done[0]) == 0 && close(job->done[1]) == 0);
}

static struct mb_node_request receive_node(struct fixture *fixture, uint64_t node,
                                            uint64_t cookie, uint32_t code)
{
    struct mb_node_request request;
    CHECK(ioctl(fixture->service, MB_IOC_RECEIVE_NODE, &request) == 0);
    CHECK(request.node == node && request.cookie == cookie && request.code == code);
    CHECK(!request.reserved && !request.buffer.reserved && request.buffer.id && request.buffer.token);
    CHECK(request.buffer.size <= MB_MAX_PAYLOAD &&
          request.buffer.offset <= MB_MAP_BYTES - request.buffer.size);
    return request;
}

static void reply_value(int fd, uint64_t id, int32_t value, int status)
{
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_transaction_reply reply = { .id = id, .data = (uintptr_t)wire, .status = status };
    reply.size = (uint32_t)encode(wire, value, 0);
    CHECK(ioctl(fd, MB_IOC_REPLY_NODE, &reply) == 0);
}

static void empty_reply(int fd, uint64_t id, int status)
{
    struct mb_transaction_reply reply = { .id = id, .status = status };
    CHECK(ioctl(fd, MB_IOC_REPLY_NODE, &reply) == 0);
}

static void check_result(struct fixture *fixture, struct job *job, int32_t value, int status)
{
    int32_t values[2];
    struct mb_transaction_call *call = &job->call.call;
    CHECK(call->token && call->reply_size <= MB_MAX_PAYLOAD);
    CHECK(call->offset <= MB_MAP_BYTES - call->reply_size && call->status == status);
    decode(fixture->replies + call->offset, call->reply_size, values);
    CHECK(values[0] == value && values[1] == 0);
    free_token(fixture->client, call->token);
}

static int32_t dispatch(struct fixture *fixture, struct object *objects, size_t count,
                         struct mb_node_request request, int *status)
{
    int32_t args[2];
    struct object *object = NULL;
    for (size_t i = 0; i < count; ++i)
        if (objects[i].id == request.node && objects[i].cookie == request.cookie)
            object = &objects[i];
    CHECK(object != NULL);
    decode(fixture->requests + request.buffer.offset, request.buffer.size, args);
    *status = 0;
    if (object->kind == 1 && request.code == 1) {
        object->value += args[0];
        return object->value;
    }
    if (object->kind == 2 && request.code == 2)
        return args[0] + args[1];
    *status = -ENOSYS;
    return 0;
}

static void publishing(void)
{
    struct fixture fixture;
    struct mb_node_publish item = { 0 };
    uint64_t ids[MB_MAX_NODES], unknown = UINT64_MAX;
    struct mb_node_publish *readonly;

    CHECK(ioctl(observer, MB_IOC_REGISTER_NODE) == -1 && errno == ENXIO);
    open_fixture(&fixture, 0);
    CHECK(ioctl(fixture.service, MB_IOC_REGISTER_NODE) == -1 && errno == EBUSY);
    CHECK(ioctl(fixture.client, MB_IOC_PUBLISH_NODE, &item) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.client, MB_IOC_REMOVE_NODE, &unknown) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, (void *)1) == -1 && errno == EFAULT);
    CHECK(ioctl(fixture.service, MB_IOC_REMOVE_NODE, (void *)1) == -1 && errno == EFAULT);
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, &item) == -1 && errno == EINVAL);
    CHECK(ioctl(observer, MB_IOC_NODE_STATS, (void *)1) == -1 && errno == EFAULT);
    CHECK(ioctl(fixture.service, MB_IOC_REMOVE_NODE, &unknown) == -1 && errno == ENOENT);
    refs(fixture.service, 0, 0, 0);
    readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(readonly != MAP_FAILED);
    *readonly = (struct mb_node_publish){ .cookie = 100 };
    CHECK(mprotect(readonly, 4096, PROT_READ) == 0);
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, readonly) == -1 && errno == EFAULT);
    refs(fixture.service, 0, 0, 0);
    ids[0] = publish(fixture.service, 100);
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, readonly) == -1 && errno == EFAULT);
    refs(fixture.service, 1, 0, 0);
    CHECK(publish(fixture.service, 100) == ids[0]);
    CHECK(munmap(readonly, 4096) == 0);
    for (unsigned int i = 1; i < MB_MAX_NODES; ++i) {
        ids[i] = publish(fixture.service, 100 + i);
        CHECK(ids[i] > ids[i - 1]);
    }
    refs(fixture.service, MB_MAX_NODES, 0, 0);
    CHECK(publish(fixture.service, 100) == ids[0]);
    item.cookie = 999;
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, &item) == -1 && errno == EAGAIN);
    remove_node(fixture.service, ids[0]);
    {
        uint64_t recovered = publish(fixture.service, 999);
        CHECK(recovered > ids[MB_MAX_NODES - 1]);
        ids[0] = recovered;
    }
    refs(fixture.service, MB_MAX_NODES, 0, 0);
    for (unsigned int i = 0; i < MB_MAX_NODES; ++i)
        remove_node(fixture.service, ids[i]);
    refs(fixture.service, 0, 0, 0);
    close_fixture(&fixture);
    puts("NODE_STAGE publish=idempotent capacity=16 output_fault=new_rollback,existing_preserved");
}

static void object_dispatch(void)
{
    struct fixture fixture;
    struct object objects[3] = {
        { .cookie = 101, .kind = 1 }, { .cookie = 102, .kind = 1 },
        { .cookie = 103, .kind = 2 }
    };
    struct job first, second;
    struct mb_node_request a, b;
    int status_a, status_b;
    int32_t value_a, value_b;

    open_fixture(&fixture, 0);
    for (unsigned int i = 0; i < 3; ++i)
        objects[i].id = publish(fixture.service, objects[i].cookie);
    for (int round = 0; round < 2; ++round) {
        start_job(&first, fixture.client, objects[0].id, 1, round ? 3 : 1, 0, 0);
        await_counts(1, 0);
        start_job(&second, fixture.client, objects[1].id, 1, round ? 20 : 10, 0, 0);
        await_counts(2, 0);
        a = receive_node(&fixture, objects[0].id, 101, 1);
        b = receive_node(&fixture, objects[1].id, 102, 1);
        value_a = dispatch(&fixture, objects, 3, a, &status_a);
        value_b = dispatch(&fixture, objects, 3, b, &status_b);
        CHECK(value_a == (round ? 4 : 1) && value_b == (round ? 30 : 10));
        free_token(fixture.service, a.buffer.token);
        free_token(fixture.service, b.buffer.token);
        reply_value(fixture.service, b.buffer.id, value_b, status_b);
        finish_job(&second, 0);
        check_result(&fixture, &second, value_b, 0);
        reply_value(fixture.service, a.buffer.id, value_a, status_a);
        finish_job(&first, 0);
        check_result(&fixture, &first, value_a, 0);
        refs(fixture.service, 3, 0, 0);
    }
    for (int unknown = 0; unknown < 2; ++unknown) {
        start_job(&first, fixture.client, objects[2].id, unknown ? 999 : 2, 19, 23, 0);
        await_counts(1, 0);
        a = receive_node(&fixture, objects[2].id, 103, unknown ? 999 : 2);
        value_a = dispatch(&fixture, objects, 3, a, &status_a);
        free_token(fixture.service, a.buffer.token);
        reply_value(fixture.service, a.buffer.id, value_a, status_a);
        finish_job(&first, 0);
        check_result(&fixture, &first, unknown ? 0 : 42, unknown ? -ENOSYS : 0);
    }
    for (unsigned int i = 0; i < 3; ++i)
        remove_node(fixture.service, objects[i].id);
    close_fixture(&fixture);
    puts("NODE_STAGE counters=independent same_code=1 values=4,30 add=42 unknown=business_ENOSYS");
}

static void holds_and_cancel(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_node_request request;
    uint64_t id, replacement, idle;
    struct mb_transaction_reply late = { 0 };

    open_fixture(&fixture, 0);
    id = publish(fixture.service, 200);
    idle = publish(fixture.service, 201);
    start_job(&job, fixture.client, id, 1, 1, 0, 0);
    await_counts(1, 0);
    refs(fixture.service, 2, 1, 1);
    busy_node(fixture.service, id);
    remove_node(fixture.service, idle);
    refs(fixture.service, 1, 1, 1);
    busy_node(fixture.service, id);
    request = receive_node(&fixture, id, 200, 1);
    busy_node(fixture.service, id);
    free_token(fixture.service, request.buffer.token);
    refs(fixture.service, 1, 1, 0);
    busy_node(fixture.service, id);
    empty_reply(fixture.service, request.buffer.id, 0);
    finish_job(&job, 0);
    refs(fixture.service, 1, 0, 0);
    remove_node(fixture.service, id);
    replacement = publish(fixture.service, 200);
    CHECK(replacement > id);
    {
        unsigned char wire[MBP_MAX_BYTES];
        struct mb_node_call stale = { .target = id, .code = 1 };
        stale.call.data = (uintptr_t)wire;
        stale.call.size = (uint32_t)encode(wire, 1, 0);
        CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &stale) == -1 && errno == ENOENT);
        CHECK(ioctl(fixture.service, MB_IOC_REMOVE_NODE, &id) == -1 && errno == ENOENT);
    }
    start_job(&job, fixture.client, replacement, 1, 2, 0, 0);
    await_counts(1, 0);
    request = receive_node(&fixture, replacement, 200, 1);
    reply_value(fixture.service, request.buffer.id, 2, 0);
    finish_job(&job, 0);
    check_result(&fixture, &job, 2, 0);
    refs(fixture.service, 1, 0, 1);
    busy_node(fixture.service, replacement);
    free_token(fixture.service, request.buffer.token);
    refs(fixture.service, 1, 0, 0);

    start_job(&job, fixture.client, replacement, 1, 4, 0, 0);
    await_counts(1, 0);
    request = receive_node(&fixture, replacement, 200, 1);
    free_token(fixture.service, request.buffer.token);
    reply_value(fixture.service, request.buffer.id, 4, 0);
    finish_job(&job, 0);
    CHECK(buffer_stats(fixture.client).used_slots == 1);
    refs(fixture.service, 1, 0, 0);
    remove_node(fixture.service, replacement);
    refs(fixture.service, 0, 0, 0);
    CHECK(buffer_stats(fixture.client).used_slots == 1);
    check_result(&fixture, &job, 4, 0);
    id = replacement;
    replacement = publish(fixture.service, 200);
    CHECK(replacement > id);

    for (int delivered = 0; delivered < 2; ++delivered) {
        start_job(&job, fixture.client, replacement, 1, 3, 0, 0);
        await_counts(1, 0);
        if (delivered)
            request = receive_node(&fixture, replacement, 200, 1);
        CHECK(pthread_kill(job.thread, SIGUSR1) == 0);
        finish_job(&job, EINTR);
        await_counts(0, 0);
        refs(fixture.service, 1, 0, (unsigned int)delivered);
        if (delivered) {
            busy_node(fixture.service, replacement);
            late.id = request.buffer.id;
            CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &late) == -1 && errno == ENOENT);
            free_token(fixture.service, request.buffer.token);
        }
    }
    remove_node(fixture.service, replacement);
    close_fixture(&fixture);
    puts("NODE_STAGE remove_busy=queued,delivered,transaction_only,slot_only cancellation=queued,delivered id_reuse=prevented");
    puts("NODE_STAGE retained_reply=does_not_pin_node read_after_remove=ok");
    puts("NODE_STAGE target_holds=isolated idle_node_remove=ok");
}

static void faults_and_modes(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_node_request request;
    struct mb_node_publish publication = { .cookie = 1 };
    struct mb_transaction_reply reply = { 0 };
    struct mb_reply old_reply = { 0 };
    struct mb_transaction_call old_call = { 0 };
    struct mb_buffer_request old_request;
    unsigned char wire[MBP_MAX_BYTES], *pages;
    struct mb_node_call call = { .code = 1 };
    uint64_t id;

    open_fixture(&fixture, 0);
    id = publish(fixture.service, 300);
    CHECK(ioctl(fixture.client, MB_IOC_REMOVE_NODE, &id) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY, &old_reply) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_TRANSACTION, &old_request) == -1 && errno == EPROTOTYPE);
    old_call.data = (uintptr_t)wire;
    old_call.size = (uint32_t)encode(wire, 1, 0);
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &old_call) == -1 && errno == EPROTOTYPE);
    call.target = id;
    call.call = old_call;
    CHECK(ioctl(fixture.service, MB_IOC_CALL_NODE, &call) == -1 && errno == EDEADLK);
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, (void *)1) == -1 && errno == EFAULT);
    call.target = 0;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EINVAL);
    call.target = UINT64_MAX;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == ENOENT);
    call.target = id;
    call.reserved = 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EINVAL);
    call.reserved = 0;
    call.call.reserved = 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EINVAL);
    call.call.reserved = 0;
    call.call.size = 0;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EINVAL);
    call.call.size = MB_MAX_PAYLOAD + 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EINVAL);
    call.call.size = old_call.size;
    call.call.data = 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EFAULT);
    pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED && mprotect(pages + 4096, 4096, PROT_NONE) == 0);
    call.call.data = (uintptr_t)(pages + 4094);
    call.call.size = 4;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EFAULT);
    refs(fixture.service, 1, 0, 0);
    CHECK(buffer_stats(fixture.service).used_slots == 0);

    start_job(&job, fixture.client, id, 1, 5, 0, 0);
    await_counts(1, 0);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_NODE, (void *)1) == -1 && errno == EFAULT);
    await_counts(1, 0);
    refs(fixture.service, 1, 1, 1);
    request = receive_node(&fixture, id, 300, 1);
    reply.id = request.buffer.id;
    CHECK(ioctl(fixture.client, MB_IOC_REPLY_NODE, &reply) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, (void *)1) == -1 && errno == EFAULT);
    reply.data = 1;
    reply.size = 4;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &reply) == -1 && errno == EFAULT);
    reply.data = (uintptr_t)(pages + 4094);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &reply) == -1 && errno == EFAULT);
    refs(fixture.service, 1, 1, 1);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    free_token(fixture.service, request.buffer.token);
    reply_value(fixture.service, request.buffer.id, 5, 0);
    finish_job(&job, 0);
    check_result(&fixture, &job, 5, 0);
    reply = (struct mb_transaction_reply){ .id = request.buffer.id };
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &reply) == -1 && errno == ENOENT);
    CHECK(munmap(pages, 8192) == 0);

    start_job(&job, fixture.client, id, 1, 6, 0, 1);
    await_counts(1, 0);
    request = receive_node(&fixture, id, 300, 1);
    free_token(fixture.service, request.buffer.token);
    reply_value(fixture.service, request.buffer.id, 6, 0);
    finish_job(&job, EFAULT);
    await_counts(0, 0);
    refs(fixture.service, 1, 0, 0);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    remove_node(fixture.service, id);
    close_fixture(&fixture);

    open_fixture(&fixture, 1);
    CHECK(ioctl(fixture.service, MB_IOC_PUBLISH_NODE, &publication) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_REMOVE_NODE, &id) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_NODE, &request) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &reply) == -1 && errno == EPROTOTYPE);
    call.target = id;
    call.call = old_call;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_NODE, &call) == -1 && errno == EPROTOTYPE);
    close_fixture(&fixture);
    puts("NODE_STAGE modes=isolated faults=call,receive,reply,output retry=ok rollback=clean");
}

static void reap_child(pid_t child, int expected_signal)
{
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(waited == child);
    active_child = -1;
    if (expected_signal)
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == expected_signal);
    else
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void client_exit_cases(void)
{
    for (int delivered = 0; delivered < 2; ++delivered) {
        struct fixture fixture;
        struct mb_node_request request = { 0 };
        struct mb_query query;
        struct mb_buffer_stats buffers;
        uint64_t id;
        int ids[2];
        pid_t child;

        open_fixture(&fixture, 0);
        id = publish(fixture.service, 400);
        CHECK(pipe(ids) == 0);
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            int fd;
            uint64_t target;
            unsigned char wire[MBP_MAX_BYTES];
            struct mb_node_call call = { .code = 1 };
            CHECK(close(ids[1]) == 0);
            CHECK(close(observer) == 0 && close(fixture.service) == 0 && close(fixture.client) == 0);
            /* The parent's receive VMAs are omitted by DONTCOPY. */
            transfer(ids[0], &target, sizeof(target), 0);
            CHECK(close(ids[0]) == 0);
            fd = open_device();
            (void)map_region(fd);
            call.target = target;
            call.call.data = (uintptr_t)wire;
            call.call.size = (uint32_t)encode(wire, 1, 0);
            (void)ioctl(fd, MB_IOC_CALL_NODE, &call);
            _exit(2);
        }
        active_child = child;
        CHECK(close(ids[0]) == 0);
        transfer(ids[1], &id, sizeof(id), 1);
        CHECK(close(ids[1]) == 0);
        await_counts(1, 0);
        refs(fixture.service, 1, 1, 1);
        if (delivered) {
            request = receive_node(&fixture, id, 400, 1);
            await_counts(0, 1);
        }
        CHECK(kill(child, SIGKILL) == 0);
        reap_child(child, SIGKILL);
        await_counts(0, 0);
        refs(fixture.service, 1, 0, (unsigned int)delivered);
        buffers = buffer_stats(fixture.service);
        CHECK(buffers.live_regions == 2 && buffers.live_slots == (unsigned int)delivered);
        CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0 && query.live_contexts == 3);
        if (delivered) {
            struct mb_transaction_reply late = { .id = request.buffer.id };
            busy_node(fixture.service, id);
            CHECK(ioctl(fixture.service, MB_IOC_REPLY_NODE, &late) == -1 && errno == ENOENT);
            free_token(fixture.service, request.buffer.token);
        }
        refs(fixture.service, 1, 0, 0);
        remove_node(fixture.service, id);
        close_fixture(&fixture);
    }
    puts("NODE_STAGE client_SIGKILL=queued,delivered transaction_refs=clean delivered_slot=explicit_free");
}

static void server_exit_cases(void)
{
    for (int delivered = 0; delivered < 2; ++delivered) {
        int client = open_device(), commands[2], reports[2];
        const unsigned char *replies = map_region(client);
        struct job job;
        uint64_t id;
        pid_t child;
        char command;

        CHECK(pipe(commands) == 0 && pipe(reports) == 0);
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            int fd;
            const unsigned char *requests;
            uint64_t node;
            CHECK(close(commands[1]) == 0 && close(reports[0]) == 0);
            CHECK(close(observer) == 0 && close(client) == 0);
            fd = open_device();
            requests = map_region(fd);
            CHECK(ioctl(fd, MB_IOC_REGISTER_NODE) == 0);
            node = publish(fd, 500);
            transfer(reports[1], &node, sizeof(node), 1);
            for (;;) {
                transfer(commands[0], &command, sizeof(command), 0);
                if (command == 'R') {
                    struct mb_node_request request;
                    int32_t args[2];
                    CHECK(ioctl(fd, MB_IOC_RECEIVE_NODE, &request) == 0);
                    CHECK(request.node == node && request.cookie == 500 && request.code == 1);
                    CHECK(request.buffer.size <= MB_MAX_PAYLOAD &&
                          request.buffer.offset <= MB_MAP_BYTES - request.buffer.size);
                    decode(requests + request.buffer.offset, request.buffer.size, args);
                    CHECK(args[0] == 1 && args[1] == 0);
                    transfer(reports[1], &request, sizeof(request), 1);
                } else {
                    CHECK(command == 'X');
                    CHECK(munmap((void *)requests, MB_MAP_BYTES) == 0 && close(fd) == 0);
                    CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
                    _exit(0);
                }
            }
        }
        active_child = child;
        CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
        transfer(reports[0], &id, sizeof(id), 0);
        CHECK(id != 0);
        start_job(&job, client, id, 1, 1, 0, 0);
        await_counts(1, 0);
        if (delivered) {
            struct mb_node_request request;
            command = 'R';
            transfer(commands[1], &command, sizeof(command), 1);
            transfer(reports[0], &request, sizeof(request), 0);
            CHECK(request.node == id && request.buffer.token);
            await_counts(0, 1);
        }
        command = 'X';
        transfer(commands[1], &command, sizeof(command), 1);
        reap_child(child, 0);
        CHECK(close(commands[1]) == 0 && close(reports[0]) == 0);
        finish_job(&job, EPIPE);
        await_counts(0, 0);
        refs(observer, 0, 0, 0);
        {
            unsigned char wire[MBP_MAX_BYTES];
            struct mb_node_call stale = { .target = id, .code = 1 };
            stale.call.data = (uintptr_t)wire;
            stale.call.size = (uint32_t)encode(wire, 1, 0);
            CHECK(ioctl(client, MB_IOC_CALL_NODE, &stale) == -1 && errno == ENOENT);
        }
        CHECK(buffer_stats(client).live_slots == 0);
        CHECK(buffer_stats(client).live_regions == 1);
        CHECK(munmap((void *)replies, MB_MAP_BYTES) == 0 && close(client) == 0);
        baseline();
    }
    puts("NODE_STAGE server_exit=queued,delivered old_target=ENOENT orphan_nodes=clean");
}

static void cross_process_objects(void)
{
    struct fixture fixture = { .service = -1, .requests = MAP_FAILED };
    uint64_t ids[2];
    int commands[2], reports[2];
    pid_t child;
    char command;

    fixture.client = open_device();
    fixture.replies = map_region(fixture.client);
    CHECK(pipe(commands) == 0 && pipe(reports) == 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct fixture service = { .client = -1, .replies = MAP_FAILED };
        struct object objects[2] = {
            { .cookie = 601, .kind = 1 }, { .cookie = 602, .kind = 1 }
        };

        CHECK(close(commands[1]) == 0 && close(reports[0]) == 0);
        CHECK(close(observer) == 0 && close(fixture.client) == 0);
        service.service = open_device();
        service.requests = map_region(service.service);
        CHECK(ioctl(service.service, MB_IOC_REGISTER_NODE) == 0);
        for (unsigned int i = 0; i < 2; ++i)
            ids[i] = objects[i].id = publish(service.service, objects[i].cookie);
        transfer(reports[1], ids, sizeof(ids), 1);
        for (int round = 0; round < 2; ++round) {
            struct mb_node_request requests[2];
            int32_t results[2];
            int statuses[2];

            transfer(commands[0], &command, 1, 0);
            CHECK(command == 'R');
            for (unsigned int i = 0; i < 2; ++i) {
                requests[i] = receive_node(&service, ids[i], objects[i].cookie, 1);
                results[i] = dispatch(&service, objects, 2, requests[i], &statuses[i]);
                CHECK(statuses[i] == 0);
                free_token(service.service, requests[i].buffer.token);
            }
            CHECK(results[0] == (round ? 4 : 1) && results[1] == (round ? 30 : 10));
            reply_value(service.service, requests[1].buffer.id, results[1], 0);
            reply_value(service.service, requests[0].buffer.id, results[0], 0);
            transfer(reports[1], results, sizeof(results), 1);
        }
        transfer(commands[0], &command, 1, 0);
        CHECK(command == 'X');
        refs(service.service, 2, 0, 0);
        remove_node(service.service, ids[0]);
        remove_node(service.service, ids[1]);
        CHECK(munmap((void *)service.requests, MB_MAP_BYTES) == 0);
        CHECK(close(service.service) == 0);
        CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
        _exit(0);
    }
    active_child = child;
    CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
    transfer(reports[0], ids, sizeof(ids), 0);
    CHECK(ids[0] && ids[1] && ids[0] != ids[1]);
    for (int round = 0; round < 2; ++round) {
        struct job first, second;
        int32_t results[2];

        start_job(&first, fixture.client, ids[0], 1, round ? 3 : 1, 0, 0);
        await_counts(1, 0);
        start_job(&second, fixture.client, ids[1], 1, round ? 20 : 10, 0, 0);
        await_counts(2, 0);
        command = 'R';
        transfer(commands[1], &command, 1, 1);
        transfer(reports[0], results, sizeof(results), 0);
        CHECK(results[0] == (round ? 4 : 1) && results[1] == (round ? 30 : 10));
        finish_job(&second, 0);
        check_result(&fixture, &second, results[1], 0);
        finish_job(&first, 0);
        check_result(&fixture, &first, results[0], 0);
        await_counts(0, 0);
    }
    command = 'X';
    transfer(commands[1], &command, 1, 1);
    reap_child(child, 0);
    CHECK(close(commands[1]) == 0 && close(reports[0]) == 0);
    refs(observer, 0, 0, 0);
    CHECK(buffer_stats(fixture.client).live_slots == 0);
    CHECK(munmap((void *)fixture.replies, MB_MAP_BYTES) == 0);
    CHECK(close(fixture.client) == 0);
    baseline();
    puts("NODE_STAGE cross_process=4 ids=pipe same_code=1 counters=4,30 reverse_replies=ok");
}

int main(void)
{
    struct sigaction action = { .sa_handler = deadline };
    struct mb_query query;
    _Static_assert(sizeof(struct mb_node_call) == 56, "node call ABI size");
    _Static_assert(sizeof(struct mb_node_request) == 56, "node request ABI size");
    _Static_assert(sizeof(struct mb_node_stats) == 24, "node stats ABI size");
    owner = getpid();
    CHECK(atexit(cleanup) == 0);
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    action.sa_handler = interrupted;
    action.sa_flags = SA_RESTART;
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0);
    alarm(45);
    observer = open_device();
    CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0 && (query.features & MB_FEATURE_NODE));
    baseline();
    publishing();
    object_dispatch();
    holds_and_cancel();
    faults_and_modes();
    client_exit_cases();
    server_exit_cases();
    cross_process_objects();
    baseline();
    CHECK(close(observer) == 0);
    observer = -1;
    alarm(0);
    puts("NODE_TEST_PASS counters=2 capacity=16 refs=validated resources=clean");
    return 0;
}
