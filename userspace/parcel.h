#ifndef MINI_BINDER_PARCEL_H
#define MINI_BINDER_PARCEL_H

#include <stddef.h>
#include <stdint.h>

#define MBP_MAX_BYTES 4096
#define MBP_VERSION 1

/* Caller-owned cursors. Use init/functions below; do not modify their fields. */
struct mbp_writer {
    unsigned char *data;
    size_t capacity;
    size_t position;
};

struct mbp_reader {
    const unsigned char *data;
    size_t size;
    size_t position;
};

/*
 * All functions return 1 on success, 0 on failure. If error is non-NULL,
 * failure stores a static reason string; success leaves *error unchanged.
 * Buffers/output pointers must reference valid caller-owned storage. Source,
 * destination and cursor/output objects must not overlap. No allocation occurs.
 * Failure leaves the caller's cursor and scalar/array outputs unchanged.
 * Writer failure does not promise to restore bytes already written.
 */

/*
 * Init writes/checks the 8-byte MBP1 + little-endian u32 version=1 header.
 * Capacity/size is at most MBP_MAX_BYTES and must include that header.
 * Success begins the cursor at offset 8. Init failure leaves it unchanged.
 * Reader borrows buffer, which must remain alive and stable while being read.
 */
int mbp_writer_init(struct mbp_writer *writer, void *buffer, size_t capacity,
                    const char **error);
int mbp_reader_init(struct mbp_reader *reader, const void *buffer, size_t size,
                    const char **error);

/* Finish publishes encoded size, or checks that no unread/trailing bytes remain. */
int mbp_writer_finish(const struct mbp_writer *writer, size_t *size,
                      const char **error);
int mbp_reader_finish(const struct mbp_reader *reader, const char **error);

/* Four-byte little-endian integers; i32 uses a two's-complement wire value. */
int mbp_write_u32(struct mbp_writer *writer, uint32_t value, const char **error);
int mbp_read_u32(struct mbp_reader *reader, uint32_t *value, const char **error);
int mbp_write_i32(struct mbp_writer *writer, int32_t value, const char **error);
int mbp_read_i32(struct mbp_reader *reader, int32_t *value, const char **error);

/*
 * Bytes: LE u32 length, data, then zero padding to four bytes. A nonzero
 * is_null selects length=UINT32_MAX and requires size=0 (data is ignored).
 * Non-NULL empty bytes use length=0; embedded NUL/non-UTF8 bytes are preserved.
 * Writer borrows data only during the call. size>0 requires data!=NULL.
 * Reader returns a const view into its input; it does not copy or append NUL.
 * NULL yields *data=NULL,*size=0,*is_null=1; empty yields *is_null=0.
 * Reader verifies zero padding before publishing any output.
 */
int mbp_write_bytes(struct mbp_writer *writer, const void *data, size_t size,
                    int is_null, const char **error);
int mbp_read_bytes(struct mbp_reader *reader, const unsigned char **data,
                   size_t *size, int *is_null, const char **error);

/*
 * Arrays: LE u32 count followed by count LE i32 elements, no NULL marker.
 * Writer borrows values for the call; count>0 requires values!=NULL.
 * Reader copies to values, whose capacity is measured in i32 elements.
 * count is output-only; insufficient capacity fails before modifying values
 * or *count. values may be NULL only when the encoded count is zero.
 */
int mbp_write_array(struct mbp_writer *writer, const int32_t *values, size_t count,
                    const char **error);
int mbp_read_array(struct mbp_reader *reader, int32_t *values, size_t capacity,
                   size_t *count, const char **error);

#endif
