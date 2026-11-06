#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "mini_binder.h"
#include "parcel.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "parcel_test pid=%ld line=%d errno=%d\n", \
                (long)getpid(), __LINE__, errno); \
        exit(1); \
    } \
} while (0)

static int service, client;
static const unsigned char *base;
static pid_t active_child = -1;
static uint64_t copies;
static uint64_t accepted_bytes;

struct sample {
    uint32_t sequence;
    int32_t status;
    const unsigned char *label;
    size_t label_size;
    int label_null;
    int32_t values[1024];
    size_t count;
};

static void deadline(int number)
{
    (void)number;
    if (active_child > 0)
        kill(active_child, SIGKILL);
    _exit(124);
}

static void clean_counts(void)
{
    struct mb_stats stats;
    struct mb_buffer_stats buffers;
    CHECK(ioctl(client, MB_IOC_STATS, &stats) == 0);
    CHECK(stats.queued == 0 && stats.delivered == 0 && stats.live_requests == 0);
    CHECK(ioctl(service, MB_IOC_BUFFER_STATS, &buffers) == 0);
    CHECK(buffers.used_slots == 0 && buffers.live_slots == 0);
    CHECK(buffers.live_regions == 1 && buffers.mapped_vmas == 1);
    CHECK(buffers.payload_copies == copies && buffers.payload_bytes == accepted_bytes);
}

static void rejected(struct mb_parcel_call call, int expected)
{
    struct mb_parcel_call before = call;
    errno = 0;
    CHECK(ioctl(client, MB_IOC_CALL_PARCEL, &call) == -1 && errno == expected);
    CHECK(memcmp(&call, &before, sizeof(call)) == 0);
    clean_counts();
}

static void offset_boundaries(void)
{
    unsigned char data[64] = { 0 };
    uint32_t offsets[MB_MAX_OBJECTS + 1] = { 0 };
    struct mb_parcel_call call = {
        .data = (uintptr_t)data, .size = sizeof(data),
        .result = 0xabcdef01U, .status = -123
    };
    void *pages;

    rejected((struct mb_parcel_call){ 0 }, EINVAL);
    call.size = MB_MAX_PAYLOAD + 1;
    rejected(call, EINVAL);
    call.size = sizeof(data);
    call.offsets = (uintptr_t)offsets;
    rejected(call, EINVAL);
    call.offsets_count = MB_MAX_OBJECTS + 1;
    rejected(call, EINVAL);
    call.offsets_count = 1;
    call.offsets = 1;
    rejected(call, EFAULT);
    call.offsets = 0;
    rejected(call, EFAULT);
    call.offsets = (uintptr_t)offsets;
    offsets[0] = 4;
    rejected(call, EINVAL);
    offsets[0] = 64;
    rejected(call, EINVAL);
    offsets[0] = UINT32_MAX;
    rejected(call, EINVAL);
    offsets[0] = UINT32_MAX - 7;
    rejected(call, EINVAL);
    offsets[0] = 0;
    call.size = 7;
    rejected(call, EINVAL);
    call.size = sizeof(data);
    call.offsets_count = 2;
    offsets[1] = 0;
    rejected(call, EINVAL);
    offsets[0] = 8;
    rejected(call, EINVAL);
    offsets[0] = 0;
    offsets[1] = 64;
    rejected(call, EINVAL);
    offsets[1] = 16;
    rejected(call, EOPNOTSUPP);
    call.offsets_count = 1;
    offsets[0] = 56;
    rejected(call, EOPNOTSUPP);
    offsets[0] = 0;
    data[0] = 0x7f;
    rejected(call, EOPNOTSUPP);
    {
        unsigned char larger[MB_MAX_OBJECTS * 8] = { 0 };
        struct mb_parcel_call full = call;
        full.data = (uintptr_t)larger;
        full.size = sizeof(larger);
        full.offsets_count = MB_MAX_OBJECTS;
        for (unsigned int i = 0; i < MB_MAX_OBJECTS; ++i)
            offsets[i] = i * 8;
        rejected(full, EOPNOTSUPP);
        offsets[MB_MAX_OBJECTS - 1] = sizeof(larger);
        rejected(full, EINVAL);
    }
    call.offsets = 0;
    call.offsets_count = 0;
    call.data = 1;
    rejected(call, EFAULT);
    CHECK(ioctl(client, MB_IOC_CALL_PARCEL, (void *)1) == -1 && errno == EFAULT);
    clean_counts();

    pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pages != MAP_FAILED);
    CHECK(mprotect((char *)pages + 4096, 4096, PROT_NONE) == 0);
    call.data = (uintptr_t)data;
    call.offsets_count = 2;
    call.offsets = (uintptr_t)((char *)pages + 4092);
    rejected(call, EFAULT);
    call.offsets = 0;
    call.offsets_count = 0;
    call.data = (uintptr_t)((char *)pages + 4092);
    rejected(call, EFAULT);
    CHECK(munmap(pages, 8192) == 0);
    puts("PARCEL_OFFSETS_PASS structural_errors=EINVAL objects=EOPNOTSUPP rollback=clean");
}

static size_t encode(unsigned char *wire, const struct sample *message)
{
    struct mbp_writer writer;
    size_t size;
    CHECK(mbp_writer_init(&writer, wire, MBP_MAX_BYTES, NULL));
    CHECK(mbp_write_u32(&writer, message->sequence, NULL));
    CHECK(mbp_write_i32(&writer, message->status, NULL));
    CHECK(mbp_write_bytes(&writer, message->label, message->label_size,
                          message->label_null, NULL));
    CHECK(mbp_write_array(&writer, message->values, message->count, NULL));
    CHECK(mbp_writer_finish(&writer, &size, NULL));
    return size;
}

static int decode(const unsigned char *wire, size_t size, struct sample *message)
{
    struct mbp_reader reader;
    struct sample result = { 0 };
    if (!mbp_reader_init(&reader, wire, size, NULL) ||
        !mbp_read_u32(&reader, &result.sequence, NULL) ||
        !mbp_read_i32(&reader, &result.status, NULL) ||
        !mbp_read_bytes(&reader, &result.label, &result.label_size,
                        &result.label_null, NULL) ||
        !mbp_read_array(&reader, result.values, 1024, &result.count, NULL) ||
        !mbp_reader_finish(&reader, NULL))
        return 0;
    *message = result;
    return 1;
}

static void mapped_message(const unsigned char *wire, size_t size,
                            const struct sample *expected, int readonly_output)
{
    struct mb_buffer_request request;
    struct sample decoded = { 0 };
    struct mb_reply reply = { 0 };
    int status, valid;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct mb_parcel_call call = {
            .data = (uintptr_t)wire, .size = (uint32_t)size,
            .result = 0xabcdef01U, .status = -123
        };
        struct mb_parcel_call *control = &call;
        int fd, result;
        close(service);
        close(client);
        fd = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        if (readonly_output) {
            control = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            CHECK(control != MAP_FAILED);
            *control = call;
            CHECK(mprotect(control, 4096, PROT_READ) == 0);
        }
        errno = 0;
        result = ioctl(fd, MB_IOC_CALL_PARCEL, control);
        if (readonly_output) {
            CHECK(result == -1 && errno == EFAULT);
            CHECK(memcmp(control, &call, sizeof(call)) == 0);
            CHECK(munmap(control, 4096) == 0);
        } else {
            CHECK(result == 0);
            CHECK(control->data == (uintptr_t)wire && control->size == size &&
                  control->offsets == 0 && control->offsets_count == 0);
            CHECK(control->result == (expected ? expected->sequence : 0));
            CHECK(control->status == (expected ? 0 : -EBADMSG));
        }
        CHECK(close(fd) == 0);
        _exit(0);
    }
    active_child = child;
    CHECK(ioctl(service, MB_IOC_RECEIVE_BUFFER, &request) == 0);
    CHECK(request.size == size && request.offset <= MB_MAP_BYTES &&
          request.size <= MB_MAP_BYTES - request.offset);
    CHECK(memcmp(base + request.offset, wire, size) == 0);
    valid = decode(base + request.offset, request.size, &decoded);
    CHECK(valid == (expected != NULL));
    if (valid) {
        CHECK(decoded.sequence == expected->sequence && decoded.status == expected->status);
        CHECK(decoded.label_null == expected->label_null &&
              decoded.label_size == expected->label_size && decoded.count == expected->count);
        if (decoded.label_size)
            CHECK(memcmp(decoded.label, expected->label, decoded.label_size) == 0);
        CHECK(memcmp(decoded.values, expected->values,
                     decoded.count * sizeof(decoded.values[0])) == 0);
        reply.result = decoded.sequence;
    } else {
        reply.status = -EBADMSG;
    }
    reply.id = request.id;
    /* No borrowed label pointer is used after this explicit buffer release. */
    CHECK(ioctl(service, MB_IOC_FREE_BUFFER, &request.token) == 0);
    CHECK(ioctl(service, MB_IOC_REPLY, &reply) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    active_child = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    copies++;
    accepted_bytes += size;
    clean_counts();
}

static void codec_matrix(void)
{
    const unsigned char label[] = { 'A', 0, 'B' };
    struct sample message = {
        .sequence = 0x12345678, .status = -7, .label = label,
        .label_size = sizeof(label), .values = { 1, -2, INT32_MAX, INT32_MIN },
        .count = 4
    };
    unsigned char wire[MBP_MAX_BYTES + 4], bad[MBP_MAX_BYTES + 4];
    unsigned char long_label[4072];
    size_t size = encode(wire, &message);
    CHECK(size == 44);
    mapped_message(wire, size, &message, 0);
    mapped_message(wire, size, &message, 1);
    for (size_t prefix = 1; prefix < size; ++prefix)
        mapped_message(wire, prefix, NULL, 0);
    memcpy(bad, wire, size);
    bad[0] = 'X';
    mapped_message(bad, size, NULL, 0);
    memcpy(bad, wire, size);
    bad[4] = 2;
    mapped_message(bad, size, NULL, 0);
    memcpy(bad, wire, size);
    bad[23] = 1;
    mapped_message(bad, size, NULL, 0);
    memcpy(bad, wire, size);
    memset(bad + 16, 0xff, 4);
    bad[16] = 0xfe;
    mapped_message(bad, size, NULL, 0);
    memcpy(bad, wire, size);
    memset(bad + 24, 0xff, 4);
    mapped_message(bad, size, NULL, 0);
    memcpy(bad, wire, size);
    bad[size] = 0;
    mapped_message(bad, size + 1, NULL, 0);
    message.label_null = 1;
    message.label_size = 0;
    message.count = 0;
    size = encode(wire, &message);
    mapped_message(wire, size, &message, 0);
    message.label_null = 0;
    size = encode(wire, &message);
    mapped_message(wire, size, &message, 0);
    memset(long_label, 0xa5, sizeof(long_label));
    message.label = long_label;
    message.label_size = sizeof(long_label);
    size = encode(wire, &message);
    CHECK(size == MBP_MAX_BYTES);
    mapped_message(wire, size, &message, 0);
    message.label_null = 1;
    message.label_size = 0;
    message.count = 1018;
    for (size_t i = 0; i < message.count; ++i)
        message.values[i] = (int32_t)i - 500;
    size = encode(wire, &message);
    CHECK(size == MBP_MAX_BYTES);
    mapped_message(wire, size, &message, 0);
    printf("PARCEL_CODEC_PASS mapped_messages=%llu every_prefix=43 business_rejects=49 output_fault=1 slots=0\n",
           (unsigned long long)copies);
}

int main(void)
{
    struct sigaction action = { .sa_handler = deadline };
    struct mb_query query;
    _Static_assert(sizeof(struct mb_parcel_call) == 32, "parcel call ABI size");
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(20);
    service = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    client = open("/dev/mini_binder", O_RDWR | O_CLOEXEC);
    CHECK(service >= 0 && client >= 0);
    CHECK(ioctl(client, MB_IOC_QUERY, &query) == 0 && (query.features & MB_FEATURE_PARCEL));
    base = mmap(NULL, MB_MAP_BYTES, PROT_READ, MAP_SHARED, service, 0);
    CHECK(base != MAP_FAILED);
    CHECK(ioctl(service, MB_IOC_REGISTER_BUFFER) == 0);
    offset_boundaries();
    codec_matrix();
    CHECK(munmap((void *)base, MB_MAP_BYTES) == 0);
    CHECK(close(service) == 0);
    {
        struct mb_buffer_stats buffers;
        CHECK(ioctl(client, MB_IOC_BUFFER_STATS, &buffers) == 0);
        CHECK(buffers.live_regions == 0 && buffers.live_slots == 0);
        CHECK(ioctl(client, MB_IOC_QUERY, &query) == 0 && query.live_contexts == 1);
    }
    CHECK(close(client) == 0);
    puts("PARCEL_TEST_PASS");
    return 0;
}
