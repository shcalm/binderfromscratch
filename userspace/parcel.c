#include "parcel.h"
#include <string.h>

static const unsigned char magic[4] = {'M', 'B', 'P', '1'};

static int fail(const char **error, const char *reason)
{
    if (error)
        *error = reason;
    return 0;
}

static int checked_add(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b)
        return 0;
    *result = a + b;
    return 1;
}

static int checked_multiply(size_t a, size_t b, size_t *result)
{
    if (b && a > SIZE_MAX / b)
        return 0;
    *result = a * b;
    return 1;
}

static int bytes_area(size_t size, int is_null, size_t *area, const char **error)
{
    size_t padded;
    if (is_null) {
        if (size)
            return fail(error, "length");
        *area = 4;
        return 1;
    }
    if (!checked_add(size, 3, &padded))
        return fail(error, "overflow");
    padded &= ~(size_t)3;
    if (!checked_add(4, padded, area))
        return fail(error, "overflow");
    if (size >= UINT32_MAX || *area > MBP_MAX_BYTES)
        return fail(error, "limit");
    return 1;
}

static int array_area(size_t count, size_t *area, const char **error)
{
    size_t bytes;
    if (!checked_multiply(count, 4, &bytes) || !checked_add(4, bytes, area))
        return fail(error, "overflow");
    if (count > UINT32_MAX || *area > MBP_MAX_BYTES)
        return fail(error, "limit");
    return 1;
}

static int writer_room(const struct mbp_writer *writer, size_t bytes,
                       const char **error)
{
    if (!writer || !writer->data)
        return fail(error, "pointer");
    if (writer->capacity > MBP_MAX_BYTES || writer->position > MBP_MAX_BYTES ||
        bytes > MBP_MAX_BYTES - writer->position)
        return fail(error, "limit");
    if (writer->position > writer->capacity || bytes > writer->capacity - writer->position)
        return fail(error, "capacity");
    return 1;
}

static int reader_room(const struct mbp_reader *reader, size_t bytes,
                       const char **error)
{
    if (!reader || !reader->data)
        return fail(error, "pointer");
    if (reader->size > MBP_MAX_BYTES)
        return fail(error, "limit");
    if (reader->position > reader->size || bytes > reader->size - reader->position)
        return fail(error, "truncated");
    return 1;
}

static uint32_t load_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int32_t signed_value(uint32_t value)
{
    if (value <= INT32_MAX)
        return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

int mbp_writer_init(struct mbp_writer *writer, void *buffer, size_t capacity,
                    const char **error)
{
    struct mbp_writer next;
    if (!writer || !buffer)
        return fail(error, "pointer");
    if (capacity > MBP_MAX_BYTES)
        return fail(error, "limit");
    if (capacity < 8)
        return fail(error, "capacity");
    next.data = buffer;
    next.capacity = capacity;
    next.position = 4;
    memcpy(next.data, magic, sizeof(magic));
    if (!mbp_write_u32(&next, MBP_VERSION, error))
        return 0;
    *writer = next;
    return 1;
}

int mbp_reader_init(struct mbp_reader *reader, const void *buffer, size_t size,
                    const char **error)
{
    const unsigned char *bytes = buffer;
    struct mbp_reader next;
    if (!reader || !buffer)
        return fail(error, "pointer");
    if (size > MBP_MAX_BYTES)
        return fail(error, "limit");
    if (size < 8)
        return fail(error, "truncated");
    if (memcmp(bytes, magic, sizeof(magic)))
        return fail(error, "magic");
    if (load_u32(bytes + 4) != MBP_VERSION)
        return fail(error, "version");
    next.data = bytes;
    next.size = size;
    next.position = 8;
    *reader = next;
    return 1;
}

int mbp_writer_finish(const struct mbp_writer *writer, size_t *size,
                      const char **error)
{
    if (!size)
        return fail(error, "pointer");
    if (!writer_room(writer, 0, error))
        return 0;
    *size = writer->position;
    return 1;
}

int mbp_reader_finish(const struct mbp_reader *reader, const char **error)
{
    if (!reader_room(reader, 0, error))
        return 0;
    if (reader->position != reader->size)
        return fail(error, "trailing");
    return 1;
}

int mbp_write_u32(struct mbp_writer *writer, uint32_t value, const char **error)
{
    if (!writer_room(writer, 4, error))
        return 0;
    for (unsigned int i = 0; i < 4; ++i)
        writer->data[writer->position + i] = (unsigned char)(value >> (i * 8));
    writer->position += 4;
    return 1;
}

int mbp_read_u32(struct mbp_reader *reader, uint32_t *value, const char **error)
{
    if (!value)
        return fail(error, "pointer");
    if (!reader_room(reader, 4, error))
        return 0;
    *value = load_u32(reader->data + reader->position);
    reader->position += 4;
    return 1;
}

int mbp_write_i32(struct mbp_writer *writer, int32_t value, const char **error)
{
    return mbp_write_u32(writer, (uint32_t)value, error);
}

int mbp_read_i32(struct mbp_reader *reader, int32_t *value, const char **error)
{
    uint32_t encoded;
    if (!value)
        return fail(error, "pointer");
    if (!mbp_read_u32(reader, &encoded, error))
        return 0;
    *value = signed_value(encoded);
    return 1;
}

int mbp_write_bytes(struct mbp_writer *writer, const void *data, size_t size,
                    int is_null, const char **error)
{
    struct mbp_writer next;
    size_t area;
    if (!bytes_area(size, is_null, &area, error) || !writer_room(writer, area, error))
        return 0;
    if (!is_null && size && !data)
        return fail(error, "pointer");
    next = *writer;
    if (!mbp_write_u32(&next, is_null ? UINT32_MAX : (uint32_t)size, error))
        return 0;
    if (!is_null) {
        if (size)
            memcpy(next.data + next.position, data, size);
        memset(next.data + next.position + size, 0, area - 4 - size);
        next.position += area - 4;
    }
    *writer = next;
    return 1;
}

int mbp_read_bytes(struct mbp_reader *reader, const unsigned char **data,
                   size_t *size, int *is_null, const char **error)
{
    struct mbp_reader next;
    uint32_t length;
    size_t area;
    int null;
    const unsigned char *result = NULL;
    if (!data || !size || !is_null || !reader)
        return fail(error, "pointer");
    next = *reader;
    if (!mbp_read_u32(&next, &length, error))
        return 0;
    null = length == UINT32_MAX;
    if (!bytes_area(null ? 0 : (size_t)length, null, &area, error) ||
        !reader_room(&next, area - 4, error))
        return 0;
    if (!null) {
        result = next.data + next.position;
        for (size_t i = length; i < area - 4; ++i)
            if (result[i])
                return fail(error, "padding");
        next.position += area - 4;
    }
    *data = result;
    *size = null ? 0 : length;
    *is_null = null;
    *reader = next;
    return 1;
}

int mbp_write_array(struct mbp_writer *writer, const int32_t *values, size_t count,
                    const char **error)
{
    struct mbp_writer next;
    size_t area;
    if (!array_area(count, &area, error) || !writer_room(writer, area, error))
        return 0;
    if (count && !values)
        return fail(error, "pointer");
    next = *writer;
    if (!mbp_write_u32(&next, (uint32_t)count, error))
        return 0;
    for (size_t i = 0; i < count; ++i)
        if (!mbp_write_i32(&next, values[i], error))
            return 0;
    *writer = next;
    return 1;
}

int mbp_read_array(struct mbp_reader *reader, int32_t *values, size_t capacity,
                   size_t *count, const char **error)
{
    struct mbp_reader next;
    uint32_t length;
    size_t area;
    if (!reader || !count)
        return fail(error, "pointer");
    next = *reader;
    if (!mbp_read_u32(&next, &length, error) || !array_area(length, &area, error) ||
        !reader_room(&next, area - 4, error))
        return 0;
    if (length > capacity)
        return fail(error, "capacity");
    if (length && !values)
        return fail(error, "pointer");
    /* The complete field is validated before the first caller output write. */
    for (size_t i = 0; i < length; ++i)
        values[i] = signed_value(load_u32(next.data + next.position + i * 4));
    next.position += area - 4;
    *count = length;
    *reader = next;
    return 1;
}
