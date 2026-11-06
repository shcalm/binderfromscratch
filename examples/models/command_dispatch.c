/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

struct model_query {
    uint32_t version;
    uint32_t features;
    uint64_t open_id;
    uint64_t live_contexts;
};

#define MODEL_QUERY _IOR('m', 0, struct model_query)
_Static_assert(sizeof(struct model_query) == 24, "query layout");

/* capacity describes trusted local storage, not a kernel user-pointer check. */
static int dispatch(unsigned long command, void *output, size_t capacity)
{
    const struct model_query result = {1, 0, 7, 1};

    if (command != MODEL_QUERY)
        return -ENOTTY;
    if (capacity < sizeof(result))
        return -EFAULT;
    memcpy(output, &result, sizeof(result));
    return 0;
}

static int rejected_case(const char *name, unsigned long command,
                         size_t capacity, int expected)
{
    unsigned char output[sizeof(struct model_query)];
    unsigned char before[sizeof(output)];
    int status;
    int unchanged;

    memset(output, 0xa5, sizeof(output));
    memcpy(before, output, sizeof(before));
    status = dispatch(command, output, capacity);
    unchanged = memcmp(output, before, sizeof(output)) == 0;
    if (status != expected || !unchanged) {
        fprintf(stderr, "case=%s unexpected_status=%d unchanged=%d\n",
                name, status, unchanged);
        return 1;
    }
    printf("case=%s status=%s unchanged=%d\n", name,
           expected == -ENOTTY ? "ENOTTY" : "EFAULT", unchanged);
    return 0;
}

int main(void)
{
    struct model_query output = {0};

    if (dispatch(MODEL_QUERY, &output, sizeof(output)) || output.version != 1 ||
        output.features != 0 || output.open_id != 7 || output.live_contexts != 1)
        return 1;
    printf("case=valid status=OK version=%u bytes=%zu\n",
           output.version, sizeof(output));
    if (rejected_case("unknown_number", _IOR('m', 1, struct model_query),
                      sizeof(output), -ENOTTY) ||
        rejected_case("wrong_encoded_size", _IOR('m', 0, uint32_t),
                      sizeof(output), -ENOTTY) ||
        rejected_case("wrong_direction", _IOW('m', 0, struct model_query),
                      sizeof(output), -ENOTTY) ||
        rejected_case("short_local_buffer", MODEL_QUERY,
                      sizeof(output) - 1, -EFAULT))
        return 1;
    puts("COMMAND_MODEL_PASS");
    return 0;
}
