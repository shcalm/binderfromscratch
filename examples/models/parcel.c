#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parcel.h"

#define PAYLOAD_LIMIT MBP_MAX_BYTES
#define VALUE_LIMIT (PAYLOAD_LIMIT / 4)
struct message {
    uint32_t sequence;
    int32_t status;
    int label_is_null;
    const unsigned char *label;
    size_t label_size;
    int32_t values[VALUE_LIMIT];
    size_t value_count;
};

static int fail(const char **error, const char *reason)
{
    *error = reason;
    return 0;
}

static int encode(struct mbp_writer *writer, const struct message *message,
                  const char **error)
{
    struct mbp_writer next = *writer;
    if (!mbp_write_u32(&next, message->sequence, error) ||
        !mbp_write_i32(&next, message->status, error) ||
        !mbp_write_bytes(&next, message->label, message->label_size,
                         message->label_is_null, error) ||
        !mbp_write_array(&next, message->values, message->value_count, error))
        return 0;
    *writer = next;
    return 1;
}

static int decode(struct mbp_reader *reader, struct message *message, const char **error)
{
    struct mbp_reader next = *reader;
    struct message result = {0};
    if (!mbp_read_u32(&next, &result.sequence, error) ||
        !mbp_read_i32(&next, &result.status, error) ||
        !mbp_read_bytes(&next, &result.label, &result.label_size, &result.label_is_null, error) ||
        !mbp_read_array(&next, result.values, VALUE_LIMIT, &result.value_count, error) ||
        !mbp_reader_finish(&next, error))
        return 0;
    *message = result;
    *reader = next;
    return 1;
}

static int hex_digit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int parse_hex(const char *text, unsigned char *bytes, size_t *size,
                     const char **error)
{
    size_t characters = strlen(text);
    if (characters % 2)
        return fail(error, "hex");
    if (characters / 2 > PAYLOAD_LIMIT)
        return fail(error, "limit");
    for (size_t i = 0; i < characters / 2; ++i) {
        int high = hex_digit(text[i * 2]), low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return fail(error, "hex");
        bytes[i] = (unsigned char)(high * 16 + low);
    }
    *size = characters / 2;
    return 1;
}

static int parse_u32(const char *text, uint32_t *value, const char **error)
{
    uintmax_t parsed;
    char *end;
    if (*text < '0' || *text > '9')
        return fail(error, "number");
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno || *end || parsed > UINT32_MAX)
        return fail(error, "number");
    *value = (uint32_t)parsed;
    return 1;
}

static int parse_i32(const char *text, const char **end, int32_t *value,
                     const char **error)
{
    const char *digits = text + (*text == '-');
    char *after;
    intmax_t parsed;
    if (*digits < '0' || *digits > '9')
        return fail(error, "number");
    errno = 0;
    parsed = strtoimax(text, &after, 10);
    if (errno || parsed < INT32_MIN || parsed > INT32_MAX)
        return fail(error, "number");
    *value = (int32_t)parsed;
    *end = after;
    return 1;
}

static int parse_values(const char *text, struct message *message, const char **error)
{
    size_t count = 0;
    while (*text) {
        const char *after;
        if (count >= VALUE_LIMIT)
            return fail(error, "limit");
        if (!parse_i32(text, &after, &message->values[count], error))
            return 0;
        ++count;
        if (!*after)
            break;
        if (*after != ',' || !after[1])
            return fail(error, "number");
        text = after + 1;
    }
    message->value_count = count;
    return 1;
}

static void print_hex(const unsigned char *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        printf("%02x", (unsigned int)data[i]);
}

static void print_message(const struct message *message)
{
    printf("sequence=%" PRIu32 "\nstatus=%" PRId32 "\nlabel=",
           message->sequence, message->status);
    if (message->label_is_null)
        printf("null");
    else
        print_hex(message->label, message->label_size);
    printf("\nvalues=");
    for (size_t i = 0; i < message->value_count; ++i)
        printf("%s%" PRId32, i ? "," : "", message->values[i]);
    putchar('\n');
}

static int selftest(const char **error)
{
    unsigned char buffer[64] = {0};
    unsigned char payload[] = {1, 2, 3};
    struct mbp_writer writer;
    struct mbp_reader reader;
    struct message message = {0};
    uint32_t value;
    int32_t values[2] = {1, -2};
    size_t size, count;
    const unsigned char *bytes;
    int null;

    if (!mbp_writer_init(&writer, buffer, 11, error) ||
        !mbp_reader_init(&reader, buffer, 11, error) ||
        mbp_write_u32(&writer, 1, error) || writer.position != 8 ||
        mbp_read_u32(&reader, &value, error) || reader.position != 8)
        return fail(error, "selftest");
    if (!mbp_writer_init(&writer, buffer, sizeof(buffer), error) ||
        mbp_write_bytes(&writer, payload, SIZE_MAX, 0, error) || writer.position != 8)
        return fail(error, "selftest");
    if (!mbp_write_u32(&writer, 3, error) ||
        !mbp_reader_init(&reader, buffer, 14, error) ||
        mbp_read_bytes(&reader, &bytes, &size, &null, error) || reader.position != 8)
        return fail(error, "selftest");
    if (!mbp_writer_init(&writer, buffer, sizeof(buffer), error) ||
        mbp_write_array(&writer, values, SIZE_MAX, error) || writer.position != 8)
        return fail(error, "selftest");
    if (!mbp_write_u32(&writer, 2, error) ||
        !mbp_reader_init(&reader, buffer, 16, error) ||
        mbp_read_array(&reader, values, 2, &count, error) || reader.position != 8)
        return fail(error, "selftest");
    if (!mbp_writer_init(&writer, buffer, 20, error) ||
        encode(&writer, &message, error) || writer.position != 8)
        return fail(error, "selftest");
    buffer[4] = 2;
    if (mbp_reader_init(&reader, buffer, 16, error) || reader.position != 8)
        return fail(error, "selftest");
    if (!mbp_writer_init(&writer, buffer, sizeof(buffer), error) ||
        !mbp_write_bytes(&writer, payload, 3, 0, error))
        return fail(error, "selftest");
    buffer[15] = 1;
    if (!mbp_reader_init(&reader, buffer, 16, error) ||
        mbp_read_bytes(&reader, &bytes, &size, &null, error) || reader.position != 8)
        return fail(error, "selftest");
    memset(buffer + 8, 0xff, 4);
    if (mbp_read_array(&reader, values, 2, &count, error) || reader.position != 8)
        return fail(error, "selftest");
    if (!mbp_writer_init(&writer, buffer, sizeof(buffer), error) ||
        mbp_write_bytes(&writer, payload, SIZE_MAX, 0, error) || strcmp(*error, "overflow") ||
        mbp_write_bytes(&writer, payload, SIZE_MAX - 1, 0, error) || strcmp(*error, "overflow") ||
        mbp_write_array(&writer, values, SIZE_MAX, error) || strcmp(*error, "overflow") ||
        mbp_write_array(&writer, values, SIZE_MAX / 4 + 1, error) || strcmp(*error, "overflow") ||
        writer.position != 8)
        return fail(error, "selftest");
    puts("cursor_checks=10\noverflow_checks=4");
    return 1;
}

static int raw_example(const char **error)
{
    struct raw_message {
        const unsigned char *label;
        size_t label_size;
        const int32_t *values;
        size_t value_count;
    };
    unsigned char label[] = {'A', 0, 'B'}, before[sizeof(label)], wire[64];
    int32_t values[] = {1, -2};
    struct raw_message original = {label, sizeof(label), values, 2}, copied;
    struct message message = {0}, decoded;
    struct mbp_writer writer;
    struct mbp_reader reader;
    size_t wire_size;

    memcpy(&copied, &original, sizeof(copied));
    memcpy(before, copied.label, sizeof(before));
    message.label = copied.label;
    message.label_size = copied.label_size;
    message.value_count = copied.value_count;
    for (size_t i = 0; i < copied.value_count; ++i)
        message.values[i] = copied.values[i];
    if (!mbp_writer_init(&writer, wire, sizeof(wire), error) ||
        !encode(&writer, &message, error) || !mbp_writer_finish(&writer, &wire_size, error))
        return 0;
    label[0] = 'Z';
    values[0] = 99;
    if (!mbp_reader_init(&reader, wire, wire_size, error) ||
        !decode(&reader, &decoded, error))
        return 0;
    printf("same_label_pointer=%d\nsame_array_pointer=%d\nraw_label_before=",
           copied.label == original.label, copied.values == original.values);
    print_hex(before, sizeof(before));
    printf("\nraw_label_after=");
    print_hex(copied.label, copied.label_size);
    printf("\nraw_array_after=%" PRId32 ",%" PRId32 "\nwire_label=",
           copied.values[0], copied.values[1]);
    print_hex(decoded.label, decoded.label_size);
    printf("\nwire_values=");
    for (size_t i = 0; i < decoded.value_count; ++i)
        printf("%s%" PRId32, i ? "," : "", decoded.values[i]);
    putchar('\n');
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char wire[PAYLOAD_LIMIT], label[PAYLOAD_LIMIT];
    struct message message = {0};
    struct mbp_writer writer;
    struct mbp_reader reader;
    size_t wire_size;
    const char *error = "input";

    if (argc == 2 && !strcmp(argv[1], "raw")) {
        if (!raw_example(&error))
            goto failure;
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        if (!selftest(&error))
            goto failure;
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "decode")) {
        if (!parse_hex(argv[2], wire, &wire_size, &error) ||
            !mbp_reader_init(&reader, wire, wire_size, &error) ||
            !decode(&reader, &message, &error))
            goto failure;
        print_message(&message);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "encode")) {
        const char *after;
        if (!parse_u32(argv[2], &message.sequence, &error) ||
            !parse_i32(argv[3], &after, &message.status, &error))
            goto failure;
        if (*after) {
            error = "number";
            goto failure;
        }
        message.label_is_null = !strcmp(argv[4], "null");
        if (!message.label_is_null &&
            !parse_hex(argv[4], label, &message.label_size, &error))
            goto failure;
        message.label = label;
        if (!parse_values(argv[5], &message, &error) ||
            !mbp_writer_init(&writer, wire, sizeof(wire), &error) ||
            !encode(&writer, &message, &error) ||
            !mbp_writer_finish(&writer, &wire_size, &error))
            goto failure;
        printf("hex=");
        print_hex(wire, wire_size);
        putchar('\n');
        return 0;
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "demo"))) {
        static const unsigned char sample[] = {'A', 0, 'B'};
        struct message decoded;
        message.sequence = UINT32_C(0x12345678);
        message.status = -7;
        message.label = sample;
        message.label_size = sizeof(sample);
        message.values[0] = 1;
        message.values[1] = -2;
        message.values[2] = INT32_MAX;
        message.values[3] = INT32_MIN;
        message.value_count = 4;
        if (!mbp_writer_init(&writer, wire, sizeof(wire), &error) ||
            !encode(&writer, &message, &error) ||
            !mbp_writer_finish(&writer, &wire_size, &error))
            goto failure;
        if (!mbp_reader_init(&reader, wire, wire_size, &error) ||
            !decode(&reader, &decoded, &error))
            goto failure;
        printf("hex=");
        print_hex(wire, wire_size);
        putchar('\n');
        print_message(&decoded);
        return 0;
    }
    fprintf(stderr, "usage: %s raw | demo | encode U32 I32 null-or-HEX CSV | decode HEX | selftest\n",
            argv[0]);
    return 2;

failure:
    fprintf(stderr, "error=%s\n", error);
    return 1;
}
