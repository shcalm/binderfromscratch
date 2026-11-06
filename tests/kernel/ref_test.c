#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mini_binder.h"

#define SIZE(type, bytes) _Static_assert(sizeof(struct type) == bytes, #type " size"); \
    _Static_assert(_Alignof(struct type) == 8, #type " alignment")
#define OFFSET(type, field, bytes) _Static_assert(offsetof(struct type, field) == bytes, #type "." #field)

SIZE(mb_ref_parcel, 24);
SIZE(mb_ref_buffer, 104);
SIZE(mb_ref_export, 40);
SIZE(mb_ref_import, 112);
SIZE(mb_ref_call, 144);
SIZE(mb_ref_request, 128);
SIZE(mb_ref_reply, 40);
SIZE(mb_ref_stats, 80);
SIZE(mb_ref_object, 24);
OFFSET(mb_ref_parcel, data, 0);
OFFSET(mb_ref_parcel, offsets, 8);
OFFSET(mb_ref_parcel, size, 16);
OFFSET(mb_ref_parcel, offsets_count, 20);
OFFSET(mb_ref_buffer, buffer, 0);
OFFSET(mb_ref_buffer, offsets_count, 32);
OFFSET(mb_ref_buffer, reserved, 36);
OFFSET(mb_ref_buffer, offsets, 40);
OFFSET(mb_ref_export, recipient, 0);
OFFSET(mb_ref_export, parcel, 8);
OFFSET(mb_ref_export, grant, 32);
OFFSET(mb_ref_import, grant, 0);
OFFSET(mb_ref_import, buffer, 8);
OFFSET(mb_ref_call, target, 0);
OFFSET(mb_ref_call, code, 4);
OFFSET(mb_ref_call, request, 8);
OFFSET(mb_ref_call, reply, 32);
OFFSET(mb_ref_call, status, 136);
OFFSET(mb_ref_call, reserved, 140);
OFFSET(mb_ref_request, buffer, 0);
OFFSET(mb_ref_request, node, 104);
OFFSET(mb_ref_request, cookie, 112);
OFFSET(mb_ref_request, code, 120);
OFFSET(mb_ref_request, reserved, 124);
OFFSET(mb_ref_reply, id, 0);
OFFSET(mb_ref_reply, parcel, 8);
OFFSET(mb_ref_reply, status, 32);
OFFSET(mb_ref_reply, reserved, 36);
OFFSET(mb_ref_stats, global_ref_entries, 0);
OFFSET(mb_ref_stats, global_user_refs, 8);
OFFSET(mb_ref_stats, global_buffer_refs, 16);
OFFSET(mb_ref_stats, global_local_objects, 24);
OFFSET(mb_ref_stats, global_pending_slots, 32);
OFFSET(mb_ref_stats, current_ref_entries, 40);
OFFSET(mb_ref_stats, current_user_refs, 48);
OFFSET(mb_ref_stats, current_buffer_refs, 56);
OFFSET(mb_ref_stats, current_local_objects, 64);
OFFSET(mb_ref_stats, current_pending_slots, 72);
OFFSET(mb_ref_object, type, 0);
OFFSET(mb_ref_object, flags, 4);
OFFSET(mb_ref_object, value, 8);
OFFSET(mb_ref_object, cookie, 16);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ref_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)
#define FAILS(expression, error) CHECK((expression) == -1 && errno == (error))

struct context { int fd; const unsigned char *base; uint64_t address; };
struct job {
    pthread_t thread;
    int fd, result, error;
    struct mb_ref_call call, *control;
    unsigned char data[384];
    uint32_t offsets[16];
};
static int observer = -1;
static pid_t owner;
static pid_t children[3];

static void cleanup(void)
{
    size_t i;
    int status;
    if (getpid() != owner)
        return;
    for (i = 0; i < 3; i++)
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            while (waitpid(children[i], &status, 0) < 0 && errno == EINTR)
                ;
            children[i] = 0;
        }
}

static void deadline(int number)
{
    size_t i;
    (void)number;
    if (getpid() == owner)
        for (i = 0; i < 3; i++)
            if (children[i] > 0)
                kill(children[i], SIGKILL);
    _exit(124);
}

static void interrupted(int number) { (void)number; }

static int open_device(void)
{
    int fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static struct context context_open(int enable)
{
    struct context c = { .fd = open_device() };
    struct mb_query q;
    c.base = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, c.fd, 0);
    CHECK(c.base != MAP_FAILED);
    CHECK(ioctl(c.fd, MB_IOC_QUERY, &q) == 0);
    c.address = q.open_id;
    if (enable)
        CHECK(ioctl(c.fd, MB_IOC_ENABLE_REF) == 0);
    return c;
}

static void context_close(struct context *c)
{
    if (c->base) {
        CHECK(munmap((void *)c->base, MB_MAP_BYTES) == 0);
        c->base = NULL;
    }
    CHECK(close(c->fd) == 0);
    c->fd = -1;
}

static struct mb_ref_stats stats(int fd)
{
    struct mb_ref_stats s;
    memset(&s, 0xa5, sizeof(s));
    CHECK(ioctl(fd, MB_IOC_REF_STATS, &s) == 0);
    return s;
}

static void units(int fd, uint64_t entries, uint64_t users, uint64_t buffers,
                  uint64_t locals, uint64_t pending)
{
    struct mb_ref_stats s = stats(fd);
    CHECK(s.current_ref_entries == entries && s.current_user_refs == users &&
          s.current_buffer_refs == buffers && s.current_local_objects == locals &&
          s.current_pending_slots == pending);
}

static void await_counts(unsigned int queued, unsigned int delivered)
{
    struct timespec begin, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    for (;;) {
        struct mb_stats s;
        CHECK(ioctl(observer, MB_IOC_STATS, &s) == 0);
        if (s.queued == queued && s.delivered == delivered &&
            s.live_requests == queued + delivered)
            return;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec - begin.tv_sec < 5);
        sched_yield();
    }
}

static void baseline(void)
{
    struct mb_query q;
    struct mb_stats t;
    struct mb_node_stats n;
    struct mb_buffer_stats b;
    struct mb_ref_stats s = stats(observer);
    await_counts(0, 0);
    CHECK(ioctl(observer, MB_IOC_QUERY, &q) == 0 && q.live_contexts == 1);
    CHECK(ioctl(observer, MB_IOC_STATS, &t) == 0 && !t.server_registered);
    CHECK(ioctl(observer, MB_IOC_NODE_STATS, &n) == 0 && !n.live_nodes);
    CHECK(ioctl(observer, MB_IOC_BUFFER_STATS, &b) == 0 && !b.live_slots && !b.live_regions);
    CHECK(!s.global_ref_entries && !s.global_user_refs && !s.global_buffer_refs &&
          !s.global_local_objects && !s.global_pending_slots);
}

static uint64_t publish(int fd, uint64_t cookie)
{
    struct mb_node_publish p = { .cookie = cookie };
    CHECK(ioctl(fd, MB_IOC_PUBLISH_NODE, &p) == 0 && p.id);
    return p.id;
}

static void free_slot(int fd, uint64_t token)
{
    CHECK(token && ioctl(fd, MB_IOC_FREE_BUFFER, &token) == 0);
}

static void acquire(int fd, uint32_t handle)
{
    CHECK(ioctl(fd, MB_IOC_ACQUIRE_REF, &handle) == 0);
}

static void release(int fd, uint32_t handle)
{
    CHECK(ioctl(fd, MB_IOC_RELEASE_REF, &handle) == 0);
}

static void put32(unsigned char *p, uint32_t value)
{
    size_t i;
    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void put64(unsigned char *p, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void object(unsigned char *p, uint32_t type, uint64_t value, uint64_t cookie)
{
    memset(p, 0, 24);
    put32(p, type);
    put64(p + 8, value);
    put64(p + 16, cookie);
}

static struct mb_ref_parcel parcel(const void *p, uint32_t size,
                                    const uint32_t *offsets, uint32_t count)
{
    struct mb_ref_parcel r = { .data = (uintptr_t)p, .size = size,
        .offsets = (uintptr_t)offsets, .offsets_count = count };
    return r;
}

static uint64_t export_data(int fd, uint64_t address, const void *data,
                             uint32_t size, const uint32_t *offsets, uint32_t count)
{
    struct mb_ref_export e = { .recipient = address,
        .parcel = parcel(data, size, offsets, count), .grant = UINT64_MAX };
    CHECK(ioctl(fd, MB_IOC_EXPORT_REF, &e) == 0 && e.grant && e.grant != UINT64_MAX);
    return e.grant;
}

static uint64_t export_object(int fd, uint64_t address, uint32_t type,
                              uint64_t value, uint64_t cookie)
{
    unsigned char data[24];
    uint32_t offset = 0;
    object(data, type, value, cookie);
    return export_data(fd, address, data, sizeof(data), &offset, 1);
}

static struct mb_ref_buffer import_buffer(int fd, uint64_t grant)
{
    struct mb_ref_import i;
    size_t n;
    memset(&i, 0xa5, sizeof(i));
    i.grant = grant;
    CHECK(ioctl(fd, MB_IOC_IMPORT_REF, &i) == 0);
    CHECK(!i.buffer.reserved && !i.buffer.buffer.reserved && !i.buffer.buffer.id);
    CHECK(i.buffer.offsets_count <= MB_MAX_OBJECTS);
    for (n = i.buffer.offsets_count; n < MB_MAX_OBJECTS; n++)
        CHECK(!i.buffer.offsets[n]);
    return i.buffer;
}

static uint32_t read_handle(const struct context *c, const struct mb_ref_buffer *b,
                             uint32_t index)
{
    const unsigned char *p;
    CHECK(index < b->offsets_count && b->buffer.offset + b->buffer.size <= MB_MAP_BYTES);
    p = c->base + b->buffer.offset + b->offsets[index];
    CHECK(p[0] == MB_OBJECT_HANDLE && !p[1] && !p[2] && !p[3] &&
          !get64(p + 16) && get64(p + 8) <= UINT32_MAX);
    CHECK(!p[4] && !p[5] && !p[6] && !p[7]);
    return (uint32_t)get64(p + 8);
}

static uint32_t grant_user(struct context *a, struct context *b,
                            uint64_t node, uint64_t cookie)
{
    uint64_t grant = export_object(a->fd, b->address, MB_OBJECT_LOCAL, node, cookie);
    struct mb_ref_buffer slot = import_buffer(b->fd, grant);
    uint32_t handle = read_handle(b, &slot, 0);
    acquire(b->fd, handle);
    free_slot(b->fd, slot.buffer.token);
    return handle;
}

static void *calling(void *opaque)
{
    struct job *j = opaque;
    errno = 0;
    j->result = ioctl(j->fd, MB_IOC_CALL_REF, j->control);
    j->error = errno;
    return NULL;
}

static void start_job(struct job *j, int fd, uint32_t handle, uint32_t code,
                       const unsigned char *data, uint32_t size, uint32_t count)
{
    size_t i;
    memset(j, 0, sizeof(*j));
    j->fd = fd;
    j->call.target = handle;
    j->call.code = code;
    if (size)
        memcpy(j->data, data, size);
    for (i = 0; i < count; i++)
        j->offsets[i] = (uint32_t)i * 24;
    j->call.request = parcel(j->data, size, count ? j->offsets : NULL, count);
    j->control = &j->call;
    CHECK(pthread_create(&j->thread, NULL, calling, j) == 0);
}

static void join_job(struct job *j)
{
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 5;
    CHECK(pthread_timedjoin_np(j->thread, NULL, &until) == 0);
}

static struct mb_ref_request receive(int fd)
{
    struct mb_ref_request r;
    size_t i;
    memset(&r, 0xa5, sizeof(r));
    CHECK(ioctl(fd, MB_IOC_RECEIVE_REF, &r) == 0);
    CHECK(!r.reserved && !r.buffer.reserved && !r.buffer.buffer.reserved);
    for (i = r.buffer.offsets_count; i < MB_MAX_OBJECTS; i++)
        CHECK(!r.buffer.offsets[i]);
    return r;
}

static void reply_empty(int fd, uint64_t id, int32_t status)
{
    struct mb_ref_reply r = { .id = id, .status = status };
    CHECK(ioctl(fd, MB_IOC_REPLY_REF, &r) == 0);
}

static void test_lifecycle(void)
{
    struct context a = context_open(1), b = context_open(1), c = context_open(1);
    struct mb_ref_buffer slot, local;
    struct mb_ref_import imp;
    struct mb_ref_export bad;
    unsigned char data[72];
    uint32_t offsets[] = { 0, 24, 48 }, handle;
    uint64_t x, y, grant;
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    x = publish(a.fd, 0x1122);
    y = publish(a.fd, 0x3344);
    grant = export_object(a.fd, b.address, MB_OBJECT_LOCAL, x, 0x1122);
    units(b.fd, 1, 0, 1, 0, 1);
    FAILS(ioctl(b.fd, MB_IOC_FREE_BUFFER, &grant), EBUSY);
    imp = (struct mb_ref_import){ .grant = grant };
    FAILS(ioctl(c.fd, MB_IOC_IMPORT_REF, &imp), ENOENT);
    /* Authorization is committed before IMPORT. */
    handle = 1;
    acquire(b.fd, handle);
    units(b.fd, 1, 1, 1, 0, 1);
    {
        void *readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(readonly != MAP_FAILED);
        memcpy(readonly, &imp, sizeof(imp));
        CHECK(mprotect(readonly, 4096, PROT_READ) == 0);
        FAILS(ioctl(b.fd, MB_IOC_IMPORT_REF, readonly), EFAULT);
        units(b.fd, 1, 1, 1, 0, 1);
        CHECK(munmap(readonly, 4096) == 0);
    }
    slot = import_buffer(b.fd, grant);
    CHECK(read_handle(&b, &slot, 0) == handle);
    FAILS(ioctl(b.fd, MB_IOC_IMPORT_REF, &imp), ENOENT);
    FAILS(ioctl(a.fd, MB_IOC_REMOVE_NODE, &x), EBUSY);
    free_slot(b.fd, slot.buffer.token);
    units(b.fd, 1, 1, 0, 0, 0);
    FAILS(ioctl(c.fd, MB_IOC_ACQUIRE_REF, &handle), EBADF);
    grant = export_object(b.fd, a.address, MB_OBJECT_HANDLE, handle, 0);
    local = import_buffer(a.fd, grant);
    CHECK(a.base[local.buffer.offset] == MB_OBJECT_LOCAL);
    CHECK(get64(a.base + local.buffer.offset + 8) == x &&
          get64(a.base + local.buffer.offset + 16) == 0x1122);
    units(a.fd, 0, 0, 0, 1, 0);
    free_slot(a.fd, local.buffer.token);
    units(a.fd, 0, 0, 0, 0, 0);
    object(data, MB_OBJECT_LOCAL, y, 0x3344);
    object(data + 24, MB_OBJECT_LOCAL, y, 0x3344);
    grant = export_data(a.fd, b.address, data, 48, offsets, 2);
    slot = import_buffer(b.fd, grant);
    CHECK(read_handle(&b, &slot, 0) == 2 && read_handle(&b, &slot, 1) == 2);
    units(b.fd, 2, 1, 2, 0, 0);
    free_slot(b.fd, slot.buffer.token);
    units(b.fd, 1, 1, 0, 0, 0);
    /* New entry and old entry rollback restore exact units and handle. */
    object(data + 24, MB_OBJECT_HANDLE, 999, 0);
    bad = (struct mb_ref_export){ .recipient = b.address,
        .parcel = parcel(data, 48, offsets, 2) };
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &bad), EBADF);
    units(b.fd, 1, 1, 0, 0, 0);
    object(data, MB_OBJECT_LOCAL, x, 0x1122);
    object(data + 24, MB_OBJECT_LOCAL, x, 0x1122);
    object(data + 48, MB_OBJECT_HANDLE, 999, 0);
    bad.parcel = parcel(data, 72, offsets, 3);
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &bad), EBADF);
    units(b.fd, 1, 1, 0, 0, 0);
    /* Export copyout failure rolls back an accepted conversion. */
    {
        void *readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(readonly != MAP_FAILED);
        bad.parcel = parcel(data, 24, offsets, 1);
        memcpy(readonly, &bad, sizeof(bad));
        CHECK(mprotect(readonly, 4096, PROT_READ) == 0);
        FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, readonly), EFAULT);
        units(b.fd, 1, 1, 0, 0, 0);
        CHECK(munmap(readonly, 4096) == 0);
    }
    release(b.fd, handle);
    FAILS(ioctl(b.fd, MB_IOC_RELEASE_REF, &handle), EBADF);
    CHECK(ioctl(a.fd, MB_IOC_REMOVE_NODE, &x) == 0);
    CHECK(ioctl(a.fd, MB_IOC_REMOVE_NODE, &y) == 0);
    context_close(&c); context_close(&b); context_close(&a);
    baseline();
    puts("REF_LIFECYCLE_PASS");
}

static void test_transactions(void)
{
    struct context a = context_open(1), b = context_open(1);
    struct job j;
    struct mb_ref_request request;
    struct mb_ref_reply reply;
    struct mb_ref_stats before;
    unsigned char data[48];
    uint32_t offsets[] = { 0, 24 }, handle;
    uint64_t x, y;
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    x = publish(a.fd, 0x111);
    y = publish(a.fd, 0x222);
    handle = grant_user(&a, &b, x, 0x111);
    object(data, MB_OBJECT_HANDLE, handle, 0);
    start_job(&j, b.fd, handle, 71, data, 24, 1);
    await_counts(1, 0);
    FAILS(ioctl(a.fd, MB_IOC_RECEIVE_REF, (void *)1), EFAULT);
    await_counts(1, 0);
    request = receive(a.fd);
    CHECK(request.node == x && request.cookie == 0x111 && request.code == 71);
    CHECK(a.base[request.buffer.buffer.offset] == MB_OBJECT_LOCAL);
    CHECK(get64(a.base + request.buffer.buffer.offset + 8) == x);
    units(a.fd, 0, 0, 0, 1, 0);
    release(b.fd, handle);
    units(b.fd, 0, 0, 0, 0, 0);
    /* Reuse handle 1 while the accepted request still targets X. */
    CHECK(grant_user(&a, &b, y, 0x222) == handle);
    FAILS(ioctl(a.fd, MB_IOC_REMOVE_NODE, &x), EBUSY);
    free_slot(a.fd, request.buffer.buffer.token);
    units(a.fd, 0, 0, 0, 0, 0);
    await_counts(0, 1);
    object(data, MB_OBJECT_LOCAL, x, 0x111);
    object(data + 24, MB_OBJECT_HANDLE, 999, 0);
    reply = (struct mb_ref_reply){ .id = request.buffer.buffer.id,
        .parcel = parcel(data, 48, offsets, 2), .status = -77 };
    {
        struct mb_ref_reply faulty = reply;
        struct mb_ref_stats original = stats(b.fd);
        struct mb_buffer_stats original_a, original_b;
        long page_size = sysconf(_SC_PAGESIZE);
        unsigned char *pages;
        int attempt;
        CHECK(page_size >= 24);
        pages = mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(pages != MAP_FAILED);
        memcpy(pages + page_size - 12, data, 12);
        CHECK(mprotect(pages + page_size, (size_t)page_size, PROT_NONE) == 0);
        CHECK(ioctl(a.fd, MB_IOC_BUFFER_STATS, &original_a) == 0);
        CHECK(ioctl(b.fd, MB_IOC_BUFFER_STATS, &original_b) == 0);
        for (attempt = 0; attempt < 2; attempt++) {
            struct mb_ref_stats after;
            struct mb_buffer_stats after_a, after_b;
            const void *source = attempt ? (void *)(pages + page_size - 12) : (void *)1;
            faulty.parcel = parcel(source, 24, offsets, 1);
            FAILS(ioctl(a.fd, MB_IOC_REPLY_REF, &faulty), EFAULT);
            after = stats(b.fd);
            CHECK(!memcmp(&original, &after, sizeof(original)));
            CHECK(ioctl(a.fd, MB_IOC_BUFFER_STATS, &after_a) == 0);
            CHECK(ioctl(b.fd, MB_IOC_BUFFER_STATS, &after_b) == 0);
            CHECK(!memcmp(&original_a, &after_a, sizeof(original_a)));
            CHECK(!memcmp(&original_b, &after_b, sizeof(original_b)));
            await_counts(0, 1);
        }
        CHECK(munmap(pages, (size_t)page_size * 2) == 0);
    }
    before = stats(b.fd);
    FAILS(ioctl(a.fd, MB_IOC_REPLY_REF, &reply), EBADF);
    {
        struct mb_ref_stats after = stats(b.fd);
        CHECK(!memcmp(&before, &after, sizeof(before)));
    }
    units(b.fd, 1, 1, 0, 0, 0);
    await_counts(0, 1);
    reply.parcel = parcel(data, 24, offsets, 1);
    CHECK(ioctl(a.fd, MB_IOC_REPLY_REF, &reply) == 0);
    join_job(&j);
    CHECK(!j.result && j.call.status == -77 && !j.call.reserved);
    CHECK(read_handle(&b, &j.call.reply, 0) == 2);
    units(b.fd, 2, 1, 1, 0, 0);
    FAILS(ioctl(a.fd, MB_IOC_REMOVE_NODE, &x), EBUSY);
    free_slot(b.fd, j.call.reply.buffer.token);
    CHECK(ioctl(a.fd, MB_IOC_REMOVE_NODE, &x) == 0);
    /* QUEUED cancellation rolls back request-local units immediately. */
    object(data, MB_OBJECT_HANDLE, handle, 0);
    start_job(&j, b.fd, handle, 72, data, 24, 1);
    await_counts(1, 0);
    units(a.fd, 0, 0, 0, 1, 0);
    CHECK(pthread_kill(j.thread, SIGUSR1) == 0);
    join_job(&j);
    CHECK(j.result == -1 && j.error == EINTR);
    await_counts(0, 0);
    units(a.fd, 0, 0, 0, 0, 0);
    /* DELIVERED cancellation preserves both target slot and objects. */
    start_job(&j, b.fd, handle, 73, data, 24, 1);
    await_counts(1, 0);
    request = receive(a.fd);
    CHECK(request.node == y);
    CHECK(pthread_kill(j.thread, SIGUSR1) == 0);
    join_job(&j);
    CHECK(j.result == -1 && j.error == EINTR);
    await_counts(0, 0);
    units(a.fd, 0, 0, 0, 1, 0);
    reply = (struct mb_ref_reply){ .id = request.buffer.buffer.id };
    FAILS(ioctl(a.fd, MB_IOC_REPLY_REF, &reply), ENOENT);
    free_slot(a.fd, request.buffer.buffer.token);
    units(a.fd, 0, 0, 0, 0, 0);
    /* Empty replies allocate no caller slot; they do not FREE requests. */
    data[0] = 9;
    start_job(&j, b.fd, handle, 74, data, 1, 0);
    await_counts(1, 0);
    request = receive(a.fd);
    reply_empty(a.fd, request.buffer.buffer.id, -123);
    join_job(&j);
    CHECK(!j.result && j.call.status == -123);
    CHECK(!memcmp(&j.call.reply, &(struct mb_ref_buffer){0}, sizeof(j.call.reply)));
    FAILS(ioctl(a.fd, MB_IOC_REMOVE_NODE, &y), EBUSY);
    free_slot(a.fd, request.buffer.buffer.token);
    /* Caller control EFAULT releases the reply's embedded object units. */
    {
        void *readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(readonly != MAP_FAILED);
        memset(&j, 0, sizeof(j));
        j.fd = b.fd;
        j.data[0] = 1;
        j.call.target = handle;
        j.call.request = parcel(j.data, 1, NULL, 0);
        memcpy(readonly, &j.call, sizeof(j.call));
        CHECK(mprotect(readonly, 4096, PROT_READ) == 0);
        j.control = readonly;
        CHECK(pthread_create(&j.thread, NULL, calling, &j) == 0);
        await_counts(1, 0);
        request = receive(a.fd);
        object(data, MB_OBJECT_LOCAL, y, 0x222);
        reply = (struct mb_ref_reply){ .id = request.buffer.buffer.id,
            .parcel = parcel(data, 24, offsets, 1) };
        CHECK(ioctl(a.fd, MB_IOC_REPLY_REF, &reply) == 0);
        join_job(&j);
        CHECK(j.result == -1 && j.error == EFAULT);
        units(b.fd, 1, 1, 0, 0, 0);
        free_slot(a.fd, request.buffer.buffer.token);
        CHECK(munmap(readonly, 4096) == 0);
    }
    release(b.fd, handle);
    CHECK(ioctl(a.fd, MB_IOC_REMOVE_NODE, &y) == 0);
    context_close(&b); context_close(&a);
    baseline();
    puts("REF_TRANSACTION_PASS");
}

enum worker_operation { W_IMPORT = 1, W_EXPORT, W_ACQUIRE, W_RELEASE,
                        W_FREE, W_CALL, W_STATS, W_EXIT };
struct command {
    uint32_t operation, handle, size, count;
    uint64_t value;
    unsigned char data[384];
    uint32_t offsets[16];
};
struct response {
    int result, error, status;
    uint64_t value;
    struct mb_ref_buffer buffer;
    struct mb_ref_stats refs;
    unsigned char data[384];
};
struct worker { pid_t pid; int command, response; uint64_t address; };
struct service_object { uint64_t id, cookie; uint32_t value; };

static void transfer(int fd, void *data, size_t size, int writing)
{
    unsigned char *p = data;
    while (size) {
        struct pollfd item = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        ssize_t n;
        int ready = poll(&item, 1, 5000);
        if (ready < 0 && errno == EINTR)
            continue;
        CHECK(ready == 1);
        n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        size -= (size_t)n;
    }
}

static void worker_loop(int input, int output)
{
    struct context c;
    struct command command;
    struct response response;
    int fd;
    for (fd = 3; fd < 1024; fd++)
        if (fd != input && fd != output)
            close(fd);
    c = context_open(1);
    transfer(output, &c.address, sizeof(c.address), 1);
    for (;;) {
        transfer(input, &command, sizeof(command), 0);
        memset(&response, 0, sizeof(response));
        errno = 0;
        switch (command.operation) {
        case W_IMPORT: {
            struct mb_ref_import i = { .grant = command.value };
            response.result = ioctl(c.fd, MB_IOC_IMPORT_REF, &i);
            response.buffer = i.buffer;
            break;
        }
        case W_EXPORT: {
            struct mb_ref_export e = { .recipient = command.value,
                .parcel = parcel(command.data, command.size,
                    command.count ? command.offsets : NULL, command.count) };
            response.result = ioctl(c.fd, MB_IOC_EXPORT_REF, &e);
            response.value = e.grant;
            break;
        }
        case W_ACQUIRE:
            response.result = ioctl(c.fd, MB_IOC_ACQUIRE_REF, &command.handle);
            break;
        case W_RELEASE:
            response.result = ioctl(c.fd, MB_IOC_RELEASE_REF, &command.handle);
            break;
        case W_FREE:
            response.result = ioctl(c.fd, MB_IOC_FREE_BUFFER, &command.value);
            break;
        case W_CALL: {
            struct mb_ref_call call = { .target = command.handle, .code = 93,
                .request = parcel(command.data, command.size,
                    command.count ? command.offsets : NULL, command.count) };
            response.result = ioctl(c.fd, MB_IOC_CALL_REF, &call);
            response.buffer = call.reply;
            response.status = call.status;
            break;
        }
        case W_STATS:
            break;
        case W_EXIT:
            context_close(&c);
            close(input); close(output);
            _exit(0);
        default:
            CHECK(0);
        }
        response.error = errno;
        if (!response.result && response.buffer.buffer.size) {
            CHECK(response.buffer.buffer.size <= sizeof(response.data));
            memcpy(response.data, c.base + response.buffer.buffer.offset,
                   response.buffer.buffer.size);
        }
        response.refs = stats(c.fd);
        transfer(output, &response, sizeof(response), 1);
    }
}

static struct worker worker_start(unsigned int index)
{
    int commands[2], responses[2];
    struct worker w;
    CHECK(index < 3 && pipe(commands) == 0 && pipe(responses) == 0);
    w.pid = fork();
    CHECK(w.pid >= 0);
    if (!w.pid)
        worker_loop(commands[0], responses[1]);
    children[index] = w.pid;
    CHECK(close(commands[0]) == 0 && close(responses[1]) == 0);
    w.command = commands[1];
    w.response = responses[0];
    transfer(w.response, &w.address, sizeof(w.address), 0);
    return w;
}

static void worker_send(struct worker *w, struct command *c)
{
    transfer(w->command, c, sizeof(*c), 1);
}

static struct response worker_read(struct worker *w)
{
    struct response r;
    transfer(w->response, &r, sizeof(r), 0);
    return r;
}

static struct response worker_run(struct worker *w, struct command *c)
{
    worker_send(w, c);
    return worker_read(w);
}

static void worker_finish(struct worker *w, unsigned int index)
{
    struct command command = { .operation = W_EXIT };
    int status;
    worker_send(w, &command);
    CHECK(waitpid(w->pid, &status, 0) == w->pid && WIFEXITED(status) && !WEXITSTATUS(status));
    children[index] = 0;
    CHECK(close(w->command) == 0 && close(w->response) == 0);
}

static struct response worker_import(struct worker *w, uint64_t grant)
{
    struct command command = { .operation = W_IMPORT, .value = grant };
    struct response r = worker_run(w, &command);
    CHECK(!r.result && !r.buffer.reserved && !r.buffer.buffer.reserved &&
          !r.buffer.buffer.id && r.buffer.offsets_count == 1 && !r.buffer.offsets[0]);
    CHECK(r.data[0] == MB_OBJECT_HANDLE && !get64(r.data + 16));
    return r;
}

static struct mb_ref_stats worker_stats(struct worker *w)
{
    struct command command = { .operation = W_STATS };
    struct response r = worker_run(w, &command);
    CHECK(!r.result);
    return r.refs;
}

static void trace_counts(const struct mb_ref_stats *s)
{
    printf("%llu/%llu/%llu", (unsigned long long)s->current_user_refs,
           (unsigned long long)s->current_buffer_refs,
           (unsigned long long)s->current_pending_slots);
}

static void worker_grant_trace(struct worker *w, uint64_t grant,
                                 const char *label, uint32_t expected)
{
    struct mb_ref_stats accepted = worker_stats(w);
    struct response imported = worker_import(w, grant), acquired, freed;
    struct command command = { .operation = W_ACQUIRE, .handle = expected };
    CHECK(get64(imported.data + 8) == expected);
    acquired = worker_run(w, &command);
    CHECK(!acquired.result);
    command = (struct command){ .operation = W_FREE, .value = imported.buffer.buffer.token };
    freed = worker_run(w, &command);
    CHECK(!freed.result);
    printf("REF_TRACE %s grant=%llu handle=%llu user/buffer/pending accepted=",
           label, (unsigned long long)grant, (unsigned long long)get64(imported.data + 8));
    trace_counts(&accepted);
    printf(" import="); trace_counts(&imported.refs);
    printf(" acquire="); trace_counts(&acquired.refs);
    printf(" free="); trace_counts(&freed.refs);
    putchar('\n');
}

static uint32_t worker_call(struct worker *w, struct context *a,
                         struct service_object objects[2], uint32_t handle,
                         uint64_t node, uint64_t cookie, uint32_t expected)
{
    struct command command = { .operation = W_CALL, .handle = handle, .size = 24, .count = 1 };
    struct mb_ref_request request;
    struct mb_ref_reply reply;
    struct service_object *selected = NULL;
    struct response r;
    unsigned char result[4];
    size_t i;
    object(command.data, MB_OBJECT_HANDLE, handle, 0);
    worker_send(w, &command);
    await_counts(1, 0);
    request = receive(a->fd);
    CHECK(request.node == node && request.cookie == cookie && request.code == 93);
    CHECK(a->base[request.buffer.buffer.offset] == MB_OBJECT_LOCAL &&
          get64(a->base + request.buffer.buffer.offset + 8) == node);
    for (i = 0; i < 2; i++)
        if (objects[i].id == request.node && objects[i].cookie == request.cookie)
            selected = &objects[i];
    CHECK(selected && request.code == 93);
    selected->value++;
    put32(result, selected->value);
    reply = (struct mb_ref_reply){ .id = request.buffer.buffer.id,
        .parcel = parcel(result, sizeof(result), NULL, 0) };
    CHECK(ioctl(a->fd, MB_IOC_REPLY_REF, &reply) == 0);
    r = worker_read(w);
    CHECK(!r.result && !r.status && r.buffer.buffer.size == sizeof(result) &&
          !r.buffer.offsets_count && get32(r.data) == expected);
    command = (struct command){ .operation = W_FREE, .value = r.buffer.buffer.token };
    CHECK(!worker_run(w, &command).result);
    free_slot(a->fd, request.buffer.buffer.token);
    return get32(r.data);
}

static void test_three_processes(void)
{
    struct context a = context_open(1);
    struct worker b, c;
    struct response r;
    struct command command;
    struct service_object objects[2];
    struct mb_ref_stats accepted;
    uint32_t first, second;
    uint64_t x, y, grant;
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    x = publish(a.fd, 0xabc);
    y = publish(a.fd, 0xdef);
    objects[0] = (struct service_object){ .id = x, .cookie = 0xabc, .value = 0 };
    objects[1] = (struct service_object){ .id = y, .cookie = 0xdef, .value = 100 };
    b = worker_start(0);
    c = worker_start(1);
    CHECK(b.pid != c.pid && b.pid != owner && c.pid != owner && b.address != c.address);
    printf("REF_TRACE A pid=%ld open=%llu B pid=%ld open=%llu C pid=%ld open=%llu\n",
           (long)owner, (unsigned long long)a.address,
           (long)b.pid, (unsigned long long)b.address,
           (long)c.pid, (unsigned long long)c.address);
    grant = export_object(a.fd, b.address, MB_OBJECT_LOCAL, x, 0xabc);
    command = (struct command){ .operation = W_IMPORT, .value = grant };
    r = worker_run(&c, &command);
    CHECK(r.result == -1 && r.error == ENOENT);
    worker_grant_trace(&b, grant, "B:X", 1);
    grant = export_object(a.fd, b.address, MB_OBJECT_LOCAL, y, 0xdef);
    worker_grant_trace(&b, grant, "B:Y", 2);
    grant = export_object(a.fd, c.address, MB_OBJECT_LOCAL, y, 0xdef);
    worker_grant_trace(&c, grant, "C:Y", 1);
    first = worker_call(&c, &a, objects, 1, y, 0xdef, 101);
    printf("REF_TRACE C:Y counter returned=%u\n", first);
    command = (struct command){ .operation = W_CALL, .handle = 2, .size = 1 };
    r = worker_run(&c, &command);
    CHECK(r.result == -1 && r.error == EBADF);
    /* Knowing X's global name/cookie cannot mint a LOCAL from B. */
    command = (struct command){ .operation = W_EXPORT, .value = c.address, .size = 24, .count = 1 };
    object(command.data, MB_OBJECT_LOCAL, x, 0xabc);
    r = worker_run(&b, &command);
    CHECK(r.result == -1 && r.error == EPERM);
    object(command.data, MB_OBJECT_HANDLE, 1, 0);
    r = worker_run(&b, &command);
    CHECK(!r.result && r.value);
    grant = r.value;
    worker_grant_trace(&c, grant, "C:X", 2);
    first = worker_call(&c, &a, objects, 2, x, 0xabc, 1);
    second = worker_call(&c, &a, objects, 2, x, 0xabc, 2);
    printf("REF_TRACE C:X counter returned=%u,%u\n", first, second);
    first = worker_call(&c, &a, objects, 1, y, 0xdef, 102);
    printf("REF_TRACE C:Y counter returned=%u\n", first);
    CHECK(objects[0].value == 2 && objects[1].value == 102);
    /* Successful grant survives the sending process's final release. */
    r = worker_run(&b, &command);
    CHECK(!r.result);
    grant = r.value;
    accepted = worker_stats(&c);
    worker_finish(&b, 0);
    r = worker_import(&c, grant);
    CHECK(get64(r.data + 8) == 2 && r.refs.current_ref_entries == 2 &&
          r.refs.current_user_refs == 2 && r.refs.current_buffer_refs == 1);
    command = (struct command){ .operation = W_FREE, .value = r.buffer.buffer.token };
    {
        struct response freed = worker_run(&c, &command);
        CHECK(!freed.result);
        printf("REF_TRACE sender-exit grant=%llu C handle=%llu accepted=",
               (unsigned long long)grant, (unsigned long long)get64(r.data + 8));
        trace_counts(&accepted);
        printf(" import="); trace_counts(&r.refs);
        printf(" free="); trace_counts(&freed.refs);
        putchar('\n');
    }
    /* An owner exit preserves pending grants and dead metadata for cleanup. */
    grant = export_object(a.fd, c.address, MB_OBJECT_LOCAL, x, 0xabc);
    accepted = worker_stats(&c);
    context_close(&a);
    r = worker_import(&c, grant);
    CHECK(get64(r.data + 8) == 2);
    printf("REF_TRACE owner-exit grant=%llu C handle=%llu accepted=",
           (unsigned long long)grant, (unsigned long long)get64(r.data + 8));
    trace_counts(&accepted);
    printf(" import="); trace_counts(&r.refs);
    putchar('\n');
    command = (struct command){ .operation = W_CALL, .handle = 2, .size = 1 };
    r = worker_run(&c, &command);
    CHECK(r.result == -1 && r.error == EPIPE);
    command = (struct command){ .operation = W_ACQUIRE, .handle = 2 };
    r = worker_run(&c, &command);
    CHECK(r.result == -1 && r.error == EPIPE);
    /* Final release cleans both explicit user and unfreed pending/import units. */
    worker_finish(&c, 1);
    baseline();
    puts("REF_THREE_PROCESS_PASS A=X,Y B=1:X,2:Y C=1:Y,2:X X=2 Y=102");
}

static void rejected_export(struct context *a, struct context *b,
                             struct mb_ref_parcel p, int error)
{
    struct mb_ref_export e = { .recipient = b->address, .parcel = p };
    struct mb_ref_stats before = stats(b->fd), after;
    struct mb_buffer_stats old_slots, new_slots;
    CHECK(ioctl(b->fd, MB_IOC_BUFFER_STATS, &old_slots) == 0);
    FAILS(ioctl(a->fd, MB_IOC_EXPORT_REF, &e), error);
    after = stats(b->fd);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    CHECK(ioctl(b->fd, MB_IOC_BUFFER_STATS, &new_slots) == 0);
    CHECK(old_slots.used_slots == new_slots.used_slots &&
          old_slots.live_slots == new_slots.live_slots &&
          old_slots.payload_copies == new_slots.payload_copies &&
          old_slots.payload_bytes == new_slots.payload_bytes);
}

static void test_validation(void)
{
    struct context a = context_open(1), b = context_open(1);
    unsigned char data[400];
    uint32_t offsets[17];
    struct mb_ref_buffer slot;
    struct mb_ref_export e;
    uint64_t x, grant;
    size_t i;
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    x = publish(a.fd, 0xf00);
    memset(data, 0, sizeof(data));
    for (i = 0; i < 16; i++) {
        offsets[i] = (uint32_t)i * 24;
        object(data + i * 24, MB_OBJECT_LOCAL, x, 0xf00);
    }
    /* Unlisted descriptor bytes stay literal and create no reference. */
    grant = export_data(a.fd, b.address, data, 24, NULL, 0);
    slot = import_buffer(b.fd, grant);
    CHECK(!slot.offsets_count && !memcmp(b.base + slot.buffer.offset, data, 24));
    units(b.fd, 0, 0, 0, 0, 0);
    free_slot(b.fd, slot.buffer.token);
    grant = export_data(a.fd, b.address, data, 384, offsets, 16);
    slot = import_buffer(b.fd, grant);
    units(b.fd, 1, 0, 16, 0, 0);
    for (i = 0; i < 16; i++)
        CHECK(read_handle(&b, &slot, (uint32_t)i) == 1);
    free_slot(b.fd, slot.buffer.token);
    rejected_export(&a, &b, parcel(data, 384, offsets, 17), EINVAL);
    rejected_export(&a, &b, parcel(data, 0, NULL, 0), EINVAL);
    rejected_export(&a, &b, parcel(data, MB_MAX_PAYLOAD + 1, NULL, 0), EINVAL);
    rejected_export(&a, &b, parcel(data, 24, offsets, 0), EINVAL);
    rejected_export(&a, &b, parcel(data, 24, NULL, 1), EFAULT);
    rejected_export(&a, &b, parcel(NULL, 24, offsets, 1), EFAULT);
    offsets[0] = 1;
    rejected_export(&a, &b, parcel(data, 48, offsets, 1), EINVAL);
    offsets[0] = 0;
    rejected_export(&a, &b, parcel(data, 7, offsets, 1), EINVAL);
    rejected_export(&a, &b, parcel(data, 8, offsets, 1), EINVAL);
    rejected_export(&a, &b, parcel(data, 23, offsets, 1), EINVAL);
    offsets[1] = 0;
    rejected_export(&a, &b, parcel(data, 48, offsets, 2), EINVAL);
    offsets[1] = 8;
    rejected_export(&a, &b, parcel(data, 48, offsets, 2), EINVAL);
    offsets[0] = 24; offsets[1] = 0;
    rejected_export(&a, &b, parcel(data, 48, offsets, 2), EINVAL);
    offsets[0] = UINT32_MAX - 7;
    rejected_export(&a, &b, parcel(data, 48, offsets, 1), EINVAL);
    offsets[0] = 0; offsets[1] = 24;
    put32(data, 99);
    rejected_export(&a, &b, parcel(data, 8, offsets, 1), EOPNOTSUPP);
    object(data, MB_OBJECT_LOCAL, x, 0xf00);
    put32(data + 4, 1);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_HANDLE, UINT64_C(1) << 32, 0);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_HANDLE, 1, 1);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_HANDLE, 0, 0);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EBADF);
    object(data, MB_OBJECT_LOCAL, 0, 0xf00);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_LOCAL, x, 0);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_LOCAL, x, 0xf01);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), EINVAL);
    object(data, MB_OBJECT_LOCAL, UINT64_MAX, 1);
    rejected_export(&a, &b, parcel(data, 24, offsets, 1), ENOENT);
    /* Full structural pass precedes semantic lookup of the first item. */
    object(data, MB_OBJECT_HANDLE, 999, 0);
    object(data + 24, MB_OBJECT_LOCAL, x, 0xf00);
    put32(data + 28, 1);
    rejected_export(&a, &b, parcel(data, 48, offsets, 2), EINVAL);
    /* Ordinary grants and object grants share exactly 16 recipient slots. */
    for (i = 0; i < MB_MAP_SLOTS; i++)
        export_data(a.fd, b.address, data, 1, NULL, 0);
    e = (struct mb_ref_export){ .recipient = b.address, .parcel = parcel(data, 1, NULL, 0) };
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), EAGAIN);
    units(b.fd, 0, 0, 0, 0, 16);
    context_close(&b); context_close(&a);
    baseline();
    puts("REF_VALIDATION_PASS");
}

static void test_modes(void)
{
    struct context a = context_open(1), b = context_open(0);
    int raw = open_device();
    unsigned char data[24];
    uint32_t offset = 0, handle = 0;
    uint64_t x, grant;
    struct mb_ref_export e;
    struct mb_ref_import imp = {0};
    struct mb_ref_call call = { .target = 0, .request = {0} };
    struct mb_node_call old = {0};
    struct mb_call plain = {0};
    struct mb_query q;
    FAILS(ioctl(raw, MB_IOC_ENABLE_REF), ENXIO);
    FAILS(ioctl(raw, MB_IOC_REGISTER_REF), EPERM);
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    x = publish(a.fd, 0xabc);
    object(data, MB_OBJECT_LOCAL, x, 0xabc);
    e = (struct mb_ref_export){ .recipient = b.address,
        .parcel = parcel(data, 24, &offset, 1) };
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), EPERM);
    e.recipient = a.address;
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), EDEADLK);
    e.recipient = UINT64_MAX;
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), ENOENT);
    e.recipient = 0;
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), EINVAL);
    FAILS(ioctl(b.fd, MB_IOC_ACQUIRE_REF, &handle), EPERM);
    CHECK(ioctl(b.fd, MB_IOC_ENABLE_REF) == 0);
    CHECK(ioctl(b.fd, MB_IOC_ENABLE_REF) == 0);
    {
        pid_t child = fork();
        int status;
        CHECK(child >= 0);
        if (!child) {
            FAILS(ioctl(b.fd, MB_IOC_ENABLE_REF), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_REGISTER_REF), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_EXPORT_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_IMPORT_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_CALL_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_ACQUIRE_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_RELEASE_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_RECEIVE_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_REPLY_REF, (void *)1), EPERM);
            FAILS(ioctl(b.fd, MB_IOC_REF_STATS, (void *)1), EPERM);
            CHECK(mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, b.fd, 0) == MAP_FAILED &&
                  errno == EPERM);
            _exit(0);
        }
        children[0] = child;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
        children[0] = 0;
    }
    FAILS(ioctl(b.fd, MB_IOC_REGISTER), EPROTOTYPE);
    FAILS(ioctl(b.fd, MB_IOC_REGISTER_BUFFER), EPROTOTYPE);
    FAILS(ioctl(b.fd, MB_IOC_REGISTER_TRANSACTION), EPROTOTYPE);
    FAILS(ioctl(b.fd, MB_IOC_REGISTER_NODE), EPROTOTYPE);
    FAILS(ioctl(b.fd, MB_IOC_CALL, &plain), EPROTOTYPE);
    old.target = x;
    old.call.size = 1;
    old.call.data = (uintptr_t)data;
    FAILS(ioctl(b.fd, MB_IOC_CALL_NODE, &old), EPROTOTYPE);
    call.request = parcel(data, 1, NULL, 0);
    FAILS(ioctl(b.fd, MB_IOC_CALL_REF, &call), EBADF);
    FAILS(ioctl(b.fd, MB_IOC_IMPORT_REF, &imp), ENOENT);
    /* A Node owned by self cannot be called through a returned LOCAL. */
    grant = export_object(a.fd, b.address, MB_OBJECT_LOCAL, x, 0xabc);
    imp.grant = grant;
    CHECK(munmap((void *)b.base, MB_MAP_BYTES) == 0);
    b.base = NULL;
    FAILS(ioctl(b.fd, MB_IOC_IMPORT_REF, &imp), ENXIO);
    FAILS(ioctl(b.fd, MB_IOC_ENABLE_REF), ENXIO);
    e.recipient = b.address;
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), ENXIO);
    CHECK(mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, b.fd, 0) == MAP_FAILED);
    handle = 1;
    acquire(b.fd, handle);
    release(b.fd, handle);
    units(b.fd, 1, 0, 1, 0, 1);
    CHECK(ioctl(raw, MB_IOC_QUERY, &q) == 0);
    CHECK(q.open_id > b.address);
    CHECK(close(raw) == 0);
    context_close(&b); context_close(&a);
    baseline();
    /* A pre-existing legacy Server rejects the new mode. */
    a = context_open(0); b = context_open(1);
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_NODE) == 0);
    FAILS(ioctl(a.fd, MB_IOC_ENABLE_REF), EPROTOTYPE);
    e.recipient = a.address;
    FAILS(ioctl(b.fd, MB_IOC_EXPORT_REF, &e), EPROTOTYPE);
    FAILS(ioctl(b.fd, MB_IOC_CALL_REF, &call), EPROTOTYPE);
    context_close(&b); context_close(&a);
    baseline();
    puts("REF_MODE_PASS");
}

static void test_death(void)
{
    int delivered;
    for (delivered = 0; delivered < 2; delivered++) {
        struct context a = context_open(1), b = context_open(1);
        struct job j;
        struct mb_ref_call dead;
        struct mb_node_stats ns;
        unsigned char data[24];
        uint64_t node;
        uint32_t handle;
        CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
        node = publish(a.fd, 700 + (uint64_t)delivered);
        handle = grant_user(&a, &b, node, 700 + (uint64_t)delivered);
        object(data, MB_OBJECT_HANDLE, handle, 0);
        start_job(&j, b.fd, handle, 88, data, 24, 1);
        await_counts(1, 0);
        if (delivered) {
            struct mb_ref_request request = receive(a.fd);
            CHECK(request.node == node);
        }
        context_close(&a);
        join_job(&j);
        CHECK(j.result == -1 && j.error == EPIPE);
        await_counts(0, 0);
        units(b.fd, 1, 1, 0, 0, 0);
        CHECK(ioctl(observer, MB_IOC_NODE_STATS, &ns) == 0 && ns.live_nodes == 1);
        CHECK(!stats(observer).global_local_objects);
        dead = (struct mb_ref_call){ .target = handle,
            .request = parcel(data, 1, NULL, 0) };
        FAILS(ioctl(b.fd, MB_IOC_CALL_REF, &dead), EPIPE);
        release(b.fd, handle);
        context_close(&b);
        baseline();
    }
    {
        struct context a = context_open(1), b = context_open(1), c = context_open(1);
        struct job j;
        struct mb_ref_request request;
        struct mb_ref_reply reply;
        struct mb_ref_export e;
        struct mb_node_stats ns;
        unsigned char data[24];
        uint32_t offset = 0, handle;
        uint64_t node;
        CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
        node = publish(a.fd, 800);
        handle = grant_user(&a, &b, node, 800);
        data[0] = 1;
        start_job(&j, b.fd, handle, 89, data, 1, 0);
        await_counts(1, 0);
        request = receive(a.fd);
        object(data, MB_OBJECT_LOCAL, node, 800);
        reply = (struct mb_ref_reply){ .id = request.buffer.buffer.id,
            .parcel = parcel(data, 24, &offset, 1) };
        CHECK(ioctl(a.fd, MB_IOC_REPLY_REF, &reply) == 0);
        join_job(&j);
        CHECK(!j.result && read_handle(&b, &j.call.reply, 0) == handle);
        free_slot(a.fd, request.buffer.buffer.token);
        context_close(&a);
        units(b.fd, 1, 1, 1, 0, 0);
        release(b.fd, handle);
        units(b.fd, 1, 0, 1, 0, 0);
        object(data, MB_OBJECT_HANDLE, handle, 0);
        e = (struct mb_ref_export){ .recipient = c.address,
            .parcel = parcel(data, 24, &offset, 1) };
        FAILS(ioctl(b.fd, MB_IOC_EXPORT_REF, &e), EPIPE);
        free_slot(b.fd, j.call.reply.buffer.token);
        CHECK(ioctl(observer, MB_IOC_NODE_STATS, &ns) == 0 && !ns.live_nodes);
        context_close(&c); context_close(&b);
        baseline();
    }
    puts("REF_DEATH_PASS");
}

static void test_capacity(void)
{
    struct context b = context_open(1), a;
    struct mb_ref_buffer slot;
    struct mb_ref_export e;
    unsigned char data[24];
    uint32_t offset = 0, handle;
    uint64_t node = 0, cookie = 0, grant;
    size_t batch, i;
    /* Dead entries allow this unique-owner chapter to reach 32 refs. */
    for (batch = 0; batch < 2; batch++) {
        a = context_open(1);
        CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
        for (i = 0; i < (batch ? 15u : 16u); i++) {
            cookie = 100 + batch * 16 + i;
            node = publish(a.fd, cookie);
            handle = grant_user(&a, &b, node, cookie);
            CHECK(handle == batch * 16 + i + 1);
        }
        context_close(&a);
    }
    units(b.fd, 31, 31, 0, 0, 0);
    handle = 1;
    FAILS(ioctl(b.fd, MB_IOC_ACQUIRE_REF, &handle), EPIPE);
    a = context_open(1);
    CHECK(ioctl(a.fd, MB_IOC_REGISTER_REF) == 0);
    node = publish(a.fd, 1000);
    CHECK(grant_user(&a, &b, node, 1000) == 32);
    units(b.fd, 32, 32, 0, 0, 0);
    grant = export_object(a.fd, b.address, MB_OBJECT_LOCAL, node, 1000);
    slot = import_buffer(b.fd, grant);
    CHECK(read_handle(&b, &slot, 0) == 32);
    units(b.fd, 32, 32, 1, 0, 0);
    free_slot(b.fd, slot.buffer.token);
    node = publish(a.fd, 1001);
    object(data, MB_OBJECT_LOCAL, node, 1001);
    e = (struct mb_ref_export){ .recipient = b.address,
        .parcel = parcel(data, 24, &offset, 1) };
    FAILS(ioctl(a.fd, MB_IOC_EXPORT_REF, &e), EAGAIN);
    units(b.fd, 32, 32, 0, 0, 0);
    handle = 1;
    release(b.fd, handle);
    CHECK(grant_user(&a, &b, node, 1001) == 1);
    units(b.fd, 32, 32, 0, 0, 0);
    /* The stale integer 1 now authorizes the newly assigned live object. */
    {
        struct job j;
        struct mb_ref_request request;
        data[0] = 1;
        start_job(&j, b.fd, 1, 131, data, 1, 0);
        await_counts(1, 0);
        request = receive(a.fd);
        CHECK(request.node == node && request.cookie == 1001);
        reply_empty(a.fd, request.buffer.buffer.id, 131);
        join_job(&j);
        CHECK(!j.result && j.call.status == 131);
        free_slot(a.fd, request.buffer.buffer.token);
    }
    FAILS(ioctl(a.fd, MB_IOC_REMOVE_NODE, &node), EBUSY);
    /* Owner-before-recipient order keeps dead refs valid for RELEASE. */
    context_close(&a);
    for (handle = 1; handle <= 32; handle++)
        release(b.fd, handle);
    units(b.fd, 0, 0, 0, 0, 0);
    context_close(&b);
    baseline();
    puts("REF_CAPACITY32_REUSE_PASS");
}

int main(void)
{
    struct sigaction action = { .sa_handler = deadline };
    owner = getpid();
    CHECK(atexit(cleanup) == 0);
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    action.sa_handler = interrupted;
    action.sa_flags = SA_RESTART;
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
    alarm(90);
    observer = open_device();
    baseline();
    test_lifecycle();
    test_transactions();
    test_three_processes();
    test_validation();
    test_modes();
    test_death();
    test_capacity();
    CHECK(close(observer) == 0);
    alarm(0);
    puts("REF_TEST_PASS");
    return 0;
}
