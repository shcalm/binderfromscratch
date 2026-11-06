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
        fprintf(stderr, "transaction_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)

struct fixture {
    int service, client;
    const unsigned char *request_base, *reply_base;
};

struct job {
    pthread_t thread;
    int fd, done[2], release[2], result, error;
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_transaction_call call;
    struct mb_transaction_call *control;
    int readonly_output;
};

struct child_server {
    pid_t pid;
    int command, report;
};

static int observer = -1;
static pid_t owner;
static volatile sig_atomic_t active_child = -1;
static uint64_t last_received_id;

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

static void token_io(int fd, int writing)
{
    char token = 'T';
    transfer(fd, &token, sizeof(token), writing);
    CHECK(token == 'T');
}

static struct mb_buffer_stats buffer_stats(int fd)
{
    struct mb_buffer_stats stats = { 0 };
    CHECK(ioctl(fd, MB_IOC_BUFFER_STATS, &stats) == 0);
    return stats;
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

static void clean_baseline(void)
{
    struct mb_query query;
    struct mb_stats stats;
    struct mb_buffer_stats buffers;

    await_counts(0, 0);
    CHECK(ioctl(observer, MB_IOC_STATS, &stats) == 0 && !stats.server_registered);
    CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0 && query.live_contexts == 1);
    buffers = buffer_stats(observer);
    CHECK(buffers.live_regions == 0 && buffers.live_slots == 0);
}

static void open_fixture(struct fixture *fixture, int legacy)
{
    fixture->service = open_device();
    fixture->client = open_device();
    fixture->request_base = map_region(fixture->service);
    fixture->reply_base = map_region(fixture->client);
    CHECK(ioctl(fixture->service, legacy ? MB_IOC_REGISTER_BUFFER :
                MB_IOC_REGISTER_TRANSACTION) == 0);
}

static void close_fixture(struct fixture *fixture)
{
    await_counts(0, 0);
    CHECK(buffer_stats(fixture->service).used_slots == 0);
    CHECK(buffer_stats(fixture->client).used_slots == 0);
    if (fixture->request_base != MAP_FAILED)
        CHECK(munmap((void *)fixture->request_base, MB_MAP_BYTES) == 0);
    if (fixture->reply_base != MAP_FAILED)
        CHECK(munmap((void *)fixture->reply_base, MB_MAP_BYTES) == 0);
    CHECK(close(fixture->service) == 0);
    CHECK(close(fixture->client) == 0);
    clean_baseline();
}

static size_t encode(unsigned char *wire, int32_t marker)
{
    const unsigned char label[] = { 'A', 0, 'B' };
    const int32_t values[] = { marker, -marker };
    struct mbp_writer writer;
    size_t size;

    CHECK(mbp_writer_init(&writer, wire, MBP_MAX_BYTES, NULL));
    CHECK(mbp_write_u32(&writer, 77, NULL));
    CHECK(mbp_write_i32(&writer, marker, NULL));
    CHECK(mbp_write_bytes(&writer, label, sizeof(label), 0, NULL));
    CHECK(mbp_write_array(&writer, values, 2, NULL));
    CHECK(mbp_writer_finish(&writer, &size, NULL));
    return size;
}

static int32_t decode(const unsigned char *wire, size_t size)
{
    const unsigned char *label;
    const unsigned char expected[] = { 'A', 0, 'B' };
    int32_t marker, values[2];
    uint32_t sequence;
    size_t label_size, count;
    int is_null;
    struct mbp_reader reader;

    CHECK(mbp_reader_init(&reader, wire, size, NULL));
    CHECK(mbp_read_u32(&reader, &sequence, NULL));
    CHECK(mbp_read_i32(&reader, &marker, NULL));
    CHECK(mbp_read_bytes(&reader, &label, &label_size, &is_null, NULL));
    CHECK(mbp_read_array(&reader, values, 2, &count, NULL));
    CHECK(mbp_reader_finish(&reader, NULL));
    CHECK(sequence == 77 && !is_null && label_size == sizeof(expected));
    CHECK(memcmp(label, expected, sizeof(expected)) == 0);
    CHECK(count == 2 && values[0] == marker && values[1] == -marker);
    return marker;
}

static void *call_thread(void *argument)
{
    struct job *job = argument;

    errno = 0;
    job->result = ioctl(job->fd, MB_IOC_CALL_TRANSACTION, job->control);
    job->error = errno;
    if (!job->result)
        job->call = *job->control;
    token_io(job->done[1], 1);
    /* Keep a completed thread alive until its owner releases and joins it. */
    token_io(job->release[0], 0);
    return NULL;
}

static void start_job(struct job *job, int fd, int32_t marker, int readonly_output)
{
    memset(job, 0, sizeof(*job));
    job->fd = fd;
    job->call.data = (uintptr_t)job->wire;
    job->call.size = (uint32_t)encode(job->wire, marker);
    job->control = &job->call;
    job->readonly_output = readonly_output;
    if (readonly_output) {
        job->control = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(job->control != MAP_FAILED);
        *job->control = job->call;
        CHECK(mprotect(job->control, 4096, PROT_READ) == 0);
    }
    CHECK(pipe(job->done) == 0 && pipe(job->release) == 0);
    CHECK(pthread_create(&job->thread, NULL, call_thread, job) == 0);
}

static void finish_job(struct job *job, int expected_error)
{
    struct timespec limit;

    token_io(job->done[0], 0);
    token_io(job->release[1], 1);
    CHECK(clock_gettime(CLOCK_REALTIME, &limit) == 0);
    limit.tv_sec += 5;
    CHECK(pthread_timedjoin_np(job->thread, NULL, &limit) == 0);
    CHECK(expected_error ? job->result == -1 && job->error == expected_error :
                          job->result == 0);
    if (job->readonly_output) {
        CHECK(memcmp(job->control, &job->call, sizeof(job->call)) == 0);
        CHECK(munmap(job->control, 4096) == 0);
    }
    CHECK(close(job->done[0]) == 0 && close(job->done[1]) == 0);
    CHECK(close(job->release[0]) == 0 && close(job->release[1]) == 0);
}

static struct mb_buffer_request receive_request(struct fixture *fixture, int32_t marker)
{
    struct mb_buffer_request request;

    CHECK(ioctl(fixture->service, MB_IOC_RECEIVE_TRANSACTION, &request) == 0);
    CHECK(request.id && request.token && !request.reserved);
    CHECK(request.size <= MB_MAX_PAYLOAD &&
          request.offset <= MB_MAP_BYTES - request.size);
    CHECK(request.offset % MB_MAX_PAYLOAD == 0);
    CHECK(decode(fixture->request_base + request.offset, request.size) == marker);
    CHECK(request.id > last_received_id);
    last_received_id = request.id;
    return request;
}

static void free_token(int fd, uint64_t token)
{
    CHECK(token != 0 && ioctl(fd, MB_IOC_FREE_BUFFER, &token) == 0);
}

static void reply_empty(int fd, uint64_t id, int status)
{
    struct mb_transaction_reply reply = { .id = id, .status = status };
    CHECK(ioctl(fd, MB_IOC_REPLY_TRANSACTION, &reply) == 0);
}

static void reply_data(int fd, uint64_t id, int32_t marker, int status)
{
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_transaction_reply reply = { .id = id, .data = (uintptr_t)wire,
                                         .status = status };
    reply.size = (uint32_t)encode(wire, marker);
    CHECK(ioctl(fd, MB_IOC_REPLY_TRANSACTION, &reply) == 0);
}

static void check_reply(struct fixture *fixture, struct job *job, int32_t marker,
                         int status)
{
    CHECK(job->call.data == (uintptr_t)job->wire && !job->call.reserved);
    CHECK(job->call.token && job->call.reply_size && job->call.reply_size <= MB_MAX_PAYLOAD);
    CHECK(job->call.offset <= MB_MAP_BYTES - job->call.reply_size);
    CHECK(job->call.offset % MB_MAX_PAYLOAD == 0 && job->call.status == status);
    CHECK(decode(fixture->reply_base + job->call.offset, job->call.reply_size) == marker);
}

static void check_empty(struct job *job, int status)
{
    CHECK(job->call.token == 0 && job->call.offset == 0 && job->call.reply_size == 0);
    CHECK(job->call.status == status);
}

static void mode_and_input_errors(void)
{
    struct fixture fixture;
    struct mb_buffer_request request;
    struct mb_reply old_reply = { 0 };
    struct mb_transaction_reply reply = { 0 };
    unsigned char data = 1;
    struct mb_transaction_call call = { .data = (uintptr_t)&data, .size = 1 };
    struct mb_call old_call = { 0 };

    CHECK(ioctl(observer, MB_IOC_REGISTER_TRANSACTION) == -1 && errno == ENXIO);
    open_fixture(&fixture, 0);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY, &old_reply) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_BUFFER, &request) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.client, MB_IOC_CALL, &old_call) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.client, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.client, MB_IOC_RECEIVE_TRANSACTION, &request) == -1 && errno == EPERM);
    CHECK(ioctl(observer, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == ENXIO);
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, (void *)1) == -1 && errno == EFAULT);
    call.size = 0;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == EINVAL);
    call.size = MB_MAX_PAYLOAD + 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == EINVAL);
    call.size = 1;
    call.reserved = 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == EINVAL);
    call.reserved = 0;
    call.data = 1;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == EFAULT);
    close_fixture(&fixture);

    open_fixture(&fixture, 1);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EPROTOTYPE);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_TRANSACTION, &request) == -1 && errno == EPROTOTYPE);
    call.data = (uintptr_t)&data;
    CHECK(ioctl(fixture.client, MB_IOC_CALL_TRANSACTION, &call) == -1 && errno == EPROTOTYPE);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE modes=isolated input_errors=validated");
}

static void reverse_replies(void)
{
    struct fixture fixture;
    struct job first, second;
    struct mb_buffer_request requests[2];
    struct mb_transaction_reply wrong = { .id = UINT64_MAX };

    open_fixture(&fixture, 0);
    start_job(&first, fixture.client, 11, 0);
    await_counts(1, 0);
    start_job(&second, fixture.client, 22, 0);
    await_counts(2, 0);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &wrong) == -1 && errno == ENOENT);
    CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_TRANSACTION, (void *)1) == -1 && errno == EFAULT);
    await_counts(2, 0);
    {
        void *readonly = mmap(NULL, 4096, PROT_READ,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(readonly != MAP_FAILED);
        CHECK(ioctl(fixture.service, MB_IOC_RECEIVE_TRANSACTION, readonly) == -1 && errno == EFAULT);
        await_counts(2, 0);
        CHECK(buffer_stats(fixture.service).used_slots == 2);
        CHECK(munmap(readonly, 4096) == 0);
    }
    requests[0] = receive_request(&fixture, 11);
    requests[1] = receive_request(&fixture, 22);
    CHECK(requests[0].id != requests[1].id && requests[0].token != requests[1].token);
    await_counts(0, 2);
    for (unsigned int i = 0; i < 2; ++i)
        CHECK(ioctl(fixture.client, MB_IOC_FREE_BUFFER, &requests[i].token) == -1 && errno == ENOENT);
    free_token(fixture.service, requests[1].token);
    CHECK(ioctl(fixture.service, MB_IOC_FREE_BUFFER, &requests[1].token) == -1 && errno == ENOENT);
    reply_data(fixture.service, requests[1].id, 220, -22);
    finish_job(&second, 0);
    check_reply(&fixture, &second, 220, -22);
    await_counts(0, 1);
    wrong.id = requests[1].id;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &wrong) == -1 && errno == ENOENT);
    CHECK(buffer_stats(fixture.service).used_slots == 1);
    CHECK(buffer_stats(fixture.client).used_slots == 1);
    reply_data(fixture.service, requests[0].id, 110, 0);
    finish_job(&first, 0);
    check_reply(&fixture, &first, 110, 0);
    CHECK(buffer_stats(fixture.service).used_slots == 1);
    CHECK(buffer_stats(fixture.client).used_slots == 2);
    CHECK(ioctl(fixture.service, MB_IOC_FREE_BUFFER, &first.call.token) == -1 && errno == ENOENT);
    CHECK(ioctl(fixture.service, MB_IOC_FREE_BUFFER, &second.call.token) == -1 && errno == ENOENT);
    free_token(fixture.service, requests[0].token);
    CHECK(ioctl(fixture.service, MB_IOC_FREE_BUFFER, &requests[0].token) == -1 && errno == ENOENT);
    free_token(fixture.client, first.call.token);
    free_token(fixture.client, second.call.token);
    CHECK(ioctl(fixture.client, MB_IOC_FREE_BUFFER, &first.call.token) == -1 && errno == ENOENT);
    CHECK(ioctl(fixture.client, MB_IOC_FREE_BUFFER, &second.call.token) == -1 && errno == ENOENT);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE same_fd_threads=2 sequence=77 reverse_replies=associated buffers=independent");
    puts("TRANSACTION_STAGE receive_EFAULT=retryable token_owner=validated duplicate_free=ENOENT");
}

static void reply_failures(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_buffer_request request;
    struct mb_transaction_reply reply;
    unsigned char *pages;
    uint64_t predicted;

    open_fixture(&fixture, 0);
    predicted = last_received_id + 1;
    start_job(&job, fixture.client, 31, 0);
    await_counts(1, 0);
    /* Isolated white-box probe; RECEIVE below confirms the predicted queued ID. */
    reply = (struct mb_transaction_reply){ .id = predicted };
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == ENOENT);
    request = receive_request(&fixture, 31);
    CHECK(request.id == predicted);
    await_counts(0, 1);
    reply.id = request.id;
    CHECK(ioctl(fixture.client, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EPERM);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, (void *)1) == -1 && errno == EFAULT);
    reply.size = MB_MAX_PAYLOAD + 1;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EINVAL);
    reply.size = 0;
    reply.data = 1;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EINVAL);
    reply.size = 1;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EFAULT);
    pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED);
    memset(pages, 0x5a, 4096);
    CHECK(mprotect(pages + 4096, 4096, PROT_NONE) == 0);
    reply.data = (uintptr_t)(pages + 4094);
    reply.size = 4;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EFAULT);
    CHECK(munmap(pages, 8192) == 0);
    await_counts(0, 1);
    CHECK(buffer_stats(fixture.service).used_slots == 1);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    reply_data(fixture.service, request.id, 310, 0);
    finish_job(&job, 0);
    check_reply(&fixture, &job, 310, 0);
    free_token(fixture.service, request.token);
    free_token(fixture.client, job.call.token);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE reply_errors=retryable partial_copy=EFAULT queued_reply=ENOENT");
}

static void reply_capacity(void)
{
    struct fixture fixture;
    struct job job;
    uint64_t tokens[MB_MAP_SLOTS];
    struct mb_buffer_request request;
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_transaction_reply reply = { .data = (uintptr_t)wire };

    open_fixture(&fixture, 0);
    for (unsigned int i = 0; i < MB_MAP_SLOTS; ++i) {
        start_job(&job, fixture.client, (int32_t)i + 1, 0);
        await_counts(1, 0);
        request = receive_request(&fixture, (int32_t)i + 1);
        free_token(fixture.service, request.token);
        reply_data(fixture.service, request.id, (int32_t)i + 100, 0);
        finish_job(&job, 0);
        check_reply(&fixture, &job, (int32_t)i + 100, 0);
        tokens[i] = job.call.token;
        CHECK(buffer_stats(fixture.client).used_slots == i + 1);
        await_counts(0, 0);
    }
    start_job(&job, fixture.client, 51, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 51);
    free_token(fixture.service, request.token);
    reply.id = request.id;
    reply.size = (uint32_t)encode(wire, 510);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == EAGAIN);
    await_counts(0, 1);
    CHECK(buffer_stats(fixture.client).used_slots == MB_MAP_SLOTS);
    free_token(fixture.client, tokens[0]);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == 0);
    finish_job(&job, 0);
    check_reply(&fixture, &job, 510, 0);
    CHECK(job.call.token != tokens[0]);
    tokens[0] = job.call.token;
    CHECK(buffer_stats(fixture.client).used_slots == MB_MAP_SLOTS);

    start_job(&job, fixture.client, 52, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 52);
    free_token(fixture.service, request.token);
    reply_empty(fixture.service, request.id, -ENOSPC);
    finish_job(&job, 0);
    check_empty(&job, -ENOSPC);
    CHECK(buffer_stats(fixture.client).used_slots == MB_MAP_SLOTS);
    for (unsigned int i = 0; i < MB_MAP_SLOTS; ++i)
        free_token(fixture.client, tokens[i]);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE reply_slots=16 full=EAGAIN free_recovery=ok empty_full_reply=ok");
}

static void mapping_and_output_failures(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_buffer_request request;
    unsigned char wire[MBP_MAX_BYTES];
    struct mb_transaction_reply reply = { .data = (uintptr_t)wire };

    open_fixture(&fixture, 0);
    start_job(&job, fixture.client, 61, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 61);
    free_token(fixture.service, request.token);
    CHECK(munmap((void *)fixture.reply_base, MB_MAP_BYTES) == 0);
    fixture.reply_base = MAP_FAILED;
    reply.id = request.id;
    reply.size = (uint32_t)encode(wire, 610);
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == -1 && errno == ENXIO);
    await_counts(0, 1);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    reply_empty(fixture.service, request.id, -ENXIO);
    finish_job(&job, 0);
    check_empty(&job, -ENXIO);
    close_fixture(&fixture);

    open_fixture(&fixture, 0);
    start_job(&job, fixture.client, 62, 1);
    await_counts(1, 0);
    request = receive_request(&fixture, 62);
    free_token(fixture.service, request.token);
    reply_data(fixture.service, request.id, 620, 0);
    finish_job(&job, EFAULT);
    await_counts(0, 0);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    start_job(&job, fixture.client, 63, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 63);
    free_token(fixture.service, request.token);
    reply_data(fixture.service, request.id, 630, 0);
    finish_job(&job, 0);
    check_reply(&fixture, &job, 630, 0);
    free_token(fixture.client, job.call.token);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE unmapped_reply=ENXIO empty_unmapped=ok output_EFAULT=rollback");
}

static void maximum_reply(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_buffer_request request;
    struct mbp_writer writer;
    unsigned char wire[MBP_MAX_BYTES], label[4072];
    size_t size;
    struct mb_transaction_reply reply = { .data = (uintptr_t)wire, .status = 7 };

    memset(label, 0xa5, sizeof(label));
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), NULL));
    CHECK(mbp_write_u32(&writer, 77, NULL));
    CHECK(mbp_write_i32(&writer, 640, NULL));
    CHECK(mbp_write_bytes(&writer, label, sizeof(label), 0, NULL));
    CHECK(mbp_write_array(&writer, NULL, 0, NULL));
    CHECK(mbp_writer_finish(&writer, &size, NULL) && size == MB_MAX_PAYLOAD);
    open_fixture(&fixture, 0);
    start_job(&job, fixture.client, 64, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 64);
    free_token(fixture.service, request.token);
    reply.id = request.id;
    reply.size = (uint32_t)size;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &reply) == 0);
    finish_job(&job, 0);
    CHECK(job.call.token && job.call.status == 7 && job.call.reply_size == size);
    CHECK(job.call.offset <= MB_MAP_BYTES - job.call.reply_size);
    CHECK(memcmp(fixture.reply_base + job.call.offset, wire, size) == 0);
    {
        struct mbp_reader reader;
        const unsigned char *view;
        uint32_t sequence;
        int32_t marker;
        size_t bytes, count;
        int is_null;

        CHECK(mbp_reader_init(&reader, fixture.reply_base + job.call.offset, size, NULL));
        CHECK(mbp_read_u32(&reader, &sequence, NULL) && sequence == 77);
        CHECK(mbp_read_i32(&reader, &marker, NULL) && marker == 640);
        CHECK(mbp_read_bytes(&reader, &view, &bytes, &is_null, NULL));
        CHECK(!is_null && bytes == sizeof(label) && memcmp(view, label, bytes) == 0);
        CHECK(mbp_read_array(&reader, NULL, 0, &count, NULL) && count == 0);
        CHECK(mbp_reader_finish(&reader, NULL));
    }
    free_token(fixture.client, job.call.token);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE reply_max=4096 codec=validated reply_oversize=EINVAL");
}

static void cancellation_orders(void)
{
    struct fixture fixture;
    struct job job;
    struct mb_buffer_request request;
    struct mb_transaction_reply late = { 0 };

    open_fixture(&fixture, 0);
    start_job(&job, fixture.client, 71, 0);
    await_counts(1, 0);
    CHECK(pthread_kill(job.thread, SIGUSR1) == 0);
    finish_job(&job, EINTR);
    await_counts(0, 0);
    CHECK(buffer_stats(fixture.service).used_slots == 0);
    CHECK(buffer_stats(fixture.client).used_slots == 0);

    start_job(&job, fixture.client, 72, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 72);
    CHECK(pthread_kill(job.thread, SIGUSR1) == 0);
    finish_job(&job, EINTR);
    await_counts(0, 0);
    CHECK(buffer_stats(fixture.service).used_slots == 1);
    CHECK(buffer_stats(fixture.client).used_slots == 0);
    late.id = request.id;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &late) == -1 && errno == ENOENT);
    free_token(fixture.service, request.token);

    start_job(&job, fixture.client, 73, 0);
    await_counts(1, 0);
    request = receive_request(&fixture, 73);
    free_token(fixture.service, request.token);
    reply_data(fixture.service, request.id, 730, 0);
    /* Successful REPLY is the explicit commit point before this signal. */
    CHECK(pthread_kill(job.thread, SIGUSR1) == 0);
    finish_job(&job, 0);
    check_reply(&fixture, &job, 730, 0);
    late.id = request.id;
    CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &late) == -1 && errno == ENOENT);
    free_token(fixture.client, job.call.token);
    close_fixture(&fixture);
    puts("TRANSACTION_STAGE cancellation=queued,delivered orders=cancel_first,reply_first terminal_once=ok");
}

static struct child_server start_child_server(int client_fd)
{
    int commands[2], reports[2];
    struct child_server server;

    CHECK(pipe(commands) == 0 && pipe(reports) == 0);
    server.pid = fork();
    CHECK(server.pid >= 0);
    if (!server.pid) {
        int fd;
        const unsigned char *base;

        CHECK(close(commands[1]) == 0 && close(reports[0]) == 0);
        CHECK(close(observer) == 0 && close(client_fd) == 0);
        fd = open_device();
        base = map_region(fd);
        CHECK(ioctl(fd, MB_IOC_REGISTER_TRANSACTION) == 0);
        token_io(reports[1], 1);
        for (;;) {
            char command;
            transfer(commands[0], &command, 1, 0);
            if (command == 'R') {
                struct mb_buffer_request request;
                CHECK(ioctl(fd, MB_IOC_RECEIVE_TRANSACTION, &request) == 0);
                CHECK(request.size <= MB_MAX_PAYLOAD &&
                      request.offset <= MB_MAP_BYTES - request.size);
                CHECK(decode(base + request.offset, request.size) == 81);
                transfer(reports[1], &request, sizeof(request), 1);
            } else {
                CHECK(command == 'X');
                CHECK(munmap((void *)base, MB_MAP_BYTES) == 0 && close(fd) == 0);
                CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
                _exit(0);
            }
        }
    }
    active_child = server.pid;
    CHECK(close(commands[0]) == 0 && close(reports[1]) == 0);
    server.command = commands[1];
    server.report = reports[0];
    token_io(server.report, 0);
    return server;
}

static void server_exit_cases(void)
{
    for (int delivered = 0; delivered < 2; ++delivered) {
        int client = open_device(), status;
        const unsigned char *base = map_region(client);
        struct child_server server = start_child_server(client);
        struct job job;
        char command = 'R';

        start_job(&job, client, 81, 0);
        await_counts(1, 0);
        if (delivered) {
            struct mb_buffer_request request;
            transfer(server.command, &command, 1, 1);
            transfer(server.report, &request, sizeof(request), 0);
            CHECK(request.id > last_received_id && request.token != 0);
            last_received_id = request.id;
            await_counts(0, 1);
        }
        command = 'X';
        transfer(server.command, &command, 1, 1);
        CHECK(waitpid(server.pid, &status, 0) == server.pid);
        active_child = -1;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(close(server.command) == 0 && close(server.report) == 0);
        finish_job(&job, EPIPE);
        await_counts(0, 0);
        CHECK(buffer_stats(client).live_slots == 0);
        CHECK(munmap((void *)base, MB_MAP_BYTES) == 0 && close(client) == 0);
        clean_baseline();
    }
    puts("TRANSACTION_STAGE server_exit=queued,delivered errors=EPIPE resources=clean");
}

static void client_exit_cases(void)
{
    for (int delivered = 0; delivered < 2; ++delivered) {
        struct fixture fixture = { .client = -1, .reply_base = MAP_FAILED };
        struct mb_buffer_request request = { 0 };
        struct mb_buffer_stats buffers;
        struct mb_query query;
        pid_t child, waited;
        int status;

        fixture.service = open_device();
        fixture.request_base = map_region(fixture.service);
        CHECK(ioctl(fixture.service, MB_IOC_REGISTER_TRANSACTION) == 0);
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            unsigned char wire[MBP_MAX_BYTES];
            struct mb_transaction_call call = { .data = (uintptr_t)wire };
            int fd;

            /* DONTCOPY omits the parent's VMA; drop inherited fd references. */
            CHECK(close(fixture.service) == 0 && close(observer) == 0);
            fd = open_device();
            (void)map_region(fd);
            call.size = (uint32_t)encode(wire, 91);
            (void)ioctl(fd, MB_IOC_CALL_TRANSACTION, &call);
            _exit(2);
        }
        active_child = child;
        await_counts(1, 0);
        if (delivered) {
            request = receive_request(&fixture, 91);
            await_counts(0, 1);
        }
        CHECK(kill(child, SIGKILL) == 0);
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        CHECK(waited == child);
        active_child = -1;
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        await_counts(0, 0);
        buffers = buffer_stats(fixture.service);
        CHECK(buffers.used_slots == (unsigned int)delivered);
        CHECK(buffers.live_slots == (unsigned int)delivered);
        CHECK(buffers.live_regions == 1 && buffers.mapped_vmas == 1);
        CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0 && query.live_contexts == 2);
        if (delivered) {
            struct mb_transaction_reply late = { .id = request.id };
            CHECK(ioctl(fixture.service, MB_IOC_REPLY_TRANSACTION, &late) == -1 && errno == ENOENT);
            free_token(fixture.service, request.token);
        }
        CHECK(buffer_stats(fixture.service).live_slots == 0);
        CHECK(munmap((void *)fixture.request_base, MB_MAP_BYTES) == 0);
        CHECK(close(fixture.service) == 0);
        clean_baseline();
    }
    puts("TRANSACTION_STAGE client_SIGKILL=queued,delivered queued_auto_free=ok delivered_explicit_free=ok");
}

int main(void)
{
    struct sigaction action = { .sa_handler = deadline };
    struct mb_query query;

    _Static_assert(sizeof(struct mb_transaction_call) == 40, "transaction call ABI size");
    _Static_assert(sizeof(struct mb_transaction_reply) == 24, "transaction reply ABI size");
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
    CHECK(ioctl(observer, MB_IOC_QUERY, &query) == 0);
    CHECK(query.features & MB_FEATURE_TRANSACTION);
    clean_baseline();
    mode_and_input_errors();
    reverse_replies();
    reply_failures();
    reply_capacity();
    mapping_and_output_failures();
    maximum_reply();
    cancellation_orders();
    server_exit_cases();
    client_exit_cases();
    clean_baseline();
    CHECK(close(observer) == 0);
    observer = -1;
    alarm(0);
    puts("TRANSACTION_TEST_PASS reverse=2 reply_slots=16 cancellation=validated resources=clean");
    return 0;
}
