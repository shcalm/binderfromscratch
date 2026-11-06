import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parcel.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "library check line=%d error=%s\n", __LINE__, error ? error : "none"); \
    return 1; } } while (0)

static int roundtrip(void)
{
    unsigned char wire[MBP_MAX_BYTES], label[] = {0xff, 0, 0x80};
    int32_t input[] = {INT32_MIN, -1, 0, INT32_MAX}, output[4] = {0};
    struct mbp_writer writer;
    struct mbp_reader reader;
    const unsigned char *view;
    const char *error = NULL;
    size_t size, label_size, count;
    uint32_t u32;
    int32_t i32;
    int null;
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_u32(&writer, UINT32_MAX, &error));
    CHECK(mbp_write_i32(&writer, INT32_MIN, &error));
    CHECK(mbp_write_bytes(&writer, label, sizeof(label), 0, &error));
    CHECK(mbp_write_array(&writer, input, 4, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error) && size == 44);
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_u32(&reader, &u32, &error) && u32 == UINT32_MAX);
    CHECK(mbp_read_i32(&reader, &i32, &error) && i32 == INT32_MIN);
    CHECK(mbp_read_bytes(&reader, &view, &label_size, &null, &error));
    CHECK(view == wire + 20 && label_size == 3 && !null && !memcmp(view, label, 3));
    CHECK(mbp_read_array(&reader, output, 4, &count, &error));
    CHECK(count == 4 && !memcmp(input, output, sizeof(input)));
    CHECK(mbp_reader_finish(&reader, &error));
    return 0;
}

static int header(void)
{
    unsigned char wire[MBP_MAX_BYTES] = {0};
    struct mbp_writer writer = {wire, 99, 77};
    struct mbp_reader reader = {wire, 99, 77};
    const char *error = NULL;
    CHECK(!mbp_writer_init(&writer, wire, 7, &error) && writer.position == 77);
    CHECK(!mbp_writer_init(&writer, wire, MBP_MAX_BYTES + 1, &error));
    CHECK(!mbp_writer_init(&writer, NULL, 8, &error));
    CHECK(writer.data == wire && writer.capacity == 99 && writer.position == 77);
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(writer.position == 8 && !memcmp(wire, "MBP1\1\0\0\0", 8));
    CHECK(!mbp_reader_init(&reader, wire, 7, &error));
    CHECK(!mbp_reader_init(&reader, wire, MBP_MAX_BYTES + 1, &error));
    CHECK(!mbp_reader_init(&reader, NULL, 8, &error));
    wire[0] = 'X';
    CHECK(!mbp_reader_init(&reader, wire, 8, &error) && !strcmp(error, "magic"));
    wire[0] = 'M';
    wire[4] = 2;
    CHECK(!mbp_reader_init(&reader, wire, 8, &error) && !strcmp(error, "version"));
    CHECK(reader.data == wire && reader.size == 99 && reader.position == 77);
    return 0;
}

static int bytes(void)
{
    unsigned char wire[64], data[] = {1, 0, 255, 2, 3, 4, 5};
    struct mbp_writer writer;
    struct mbp_reader reader;
    const unsigned char *view;
    const char *error = NULL;
    size_t size, count;
    int null;
    for (unsigned int n = 0; n <= sizeof(data); ++n) {
        CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
        CHECK(mbp_write_bytes(&writer, data, n, 0, &error));
        CHECK(mbp_writer_finish(&writer, &size, &error));
        CHECK(mbp_reader_init(&reader, wire, size, &error));
        CHECK(mbp_read_bytes(&reader, &view, &count, &null, &error));
        CHECK(view == wire + 12 && count == n && !null && !memcmp(view, data, n));
        CHECK(mbp_reader_finish(&reader, &error));
    }
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_bytes(&writer, NULL, 0, 1, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error) && size == 12);
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_bytes(&reader, &view, &count, &null, &error));
    CHECK(view == NULL && count == 0 && null == 1);
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_bytes(&writer, data, 3, 0, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error));
    wire[15] = 1;
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    view = data;
    count = 99;
    null = 7;
    CHECK(!mbp_read_bytes(&reader, &view, &count, &null, &error));
    CHECK(!strcmp(error, "padding") && reader.position == 8);
    CHECK(view == data && count == 99 && null == 7);
    return 0;
}

static int array_capacity(void)
{
    unsigned char wire[64];
    int32_t input[] = {INT32_MIN, INT32_MAX};
    struct { uint32_t before; int32_t data[2]; uint32_t after; } output =
        {UINT32_C(0x12345678), {123, 456}, UINT32_C(0xabcdef01)};
    struct mbp_writer writer;
    struct mbp_reader reader;
    const char *error = NULL;
    size_t size, count = 77;
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_array(&writer, input, 2, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error));
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(!mbp_read_array(&reader, output.data, 1, &count, &error));
    CHECK(!strcmp(error, "capacity") && count == 77 && reader.position == 8);
    CHECK(output.data[0] == 123 && output.data[1] == 456);
    CHECK(output.before == UINT32_C(0x12345678) && output.after == UINT32_C(0xabcdef01));
    CHECK(mbp_reader_init(&reader, wire, size - 1, &error));
    CHECK(!mbp_read_array(&reader, output.data, 2, &count, &error));
    CHECK(output.data[0] == 123 && output.data[1] == 456 && count == 77 && reader.position == 8);
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_array(&reader, output.data, 2, &count, &error));
    CHECK(count == 2 && !memcmp(input, output.data, sizeof(input)));
    CHECK(output.before == UINT32_C(0x12345678) && output.after == UINT32_C(0xabcdef01));
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_array(&writer, NULL, 0, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error));
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_array(&reader, NULL, 0, &count, &error) && count == 0);
    return 0;
}

static int failure_cursors(void)
{
    unsigned char wire[64], value[] = {1, 2, 3};
    int32_t items[] = {1, 2}, signed_output = 123;
    uint32_t output = 123;
    struct mbp_writer writer;
    struct mbp_reader reader;
    const char *error = NULL;
    CHECK(mbp_writer_init(&writer, wire, 11, &error));
    CHECK(!mbp_write_u32(&writer, 1, &error) && writer.position == 8);
    CHECK(!mbp_write_i32(&writer, -1, &error) && writer.position == 8);
    CHECK(mbp_reader_init(&reader, wire, 11, &error));
    CHECK(!mbp_read_u32(&reader, &output, &error) && output == 123 && reader.position == 8);
    CHECK(!mbp_read_i32(&reader, &signed_output, &error) && signed_output == 123 && reader.position == 8);
    CHECK(mbp_writer_init(&writer, wire, 15, &error));
    CHECK(!mbp_write_bytes(&writer, value, 3, 0, &error) && writer.position == 8);
    CHECK(mbp_writer_init(&writer, wire, 19, &error));
    CHECK(!mbp_write_array(&writer, items, 2, &error) && writer.position == 8);
    CHECK(!mbp_write_array(&writer, NULL, 1, &error) && writer.position == 8);
    CHECK(!mbp_write_bytes(&writer, NULL, 1, 0, &error) && writer.position == 8);
    CHECK(!mbp_write_bytes(&writer, NULL, 1, 1, &error) && writer.position == 8);
    return 0;
}

static int limits(void)
{
    unsigned char wire[MBP_MAX_BYTES], data[MBP_MAX_BYTES] = {0};
    int32_t values[MBP_MAX_BYTES / 4] = {0}, output[MBP_MAX_BYTES / 4];
    struct mbp_writer writer;
    struct mbp_reader reader;
    const unsigned char *view;
    const char *error = NULL;
    size_t size, count;
    int null;
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(mbp_write_bytes(&writer, data, 4084, 0, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error) && size == MBP_MAX_BYTES);
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_bytes(&reader, &view, &count, &null, &error) && count == 4084);
    CHECK(mbp_reader_finish(&reader, &error));
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(!mbp_write_bytes(&writer, data, 4085, 0, &error) && writer.position == 8);
    CHECK(mbp_write_array(&writer, values, 1021, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error) && size == MBP_MAX_BYTES);
    CHECK(mbp_reader_init(&reader, wire, size, &error));
    CHECK(mbp_read_array(&reader, output, 1024, &count, &error) && count == 1021);
    CHECK(mbp_reader_finish(&reader, &error));
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), &error));
    CHECK(!mbp_write_array(&writer, values, 1022, &error) && writer.position == 8);
    CHECK(!mbp_write_bytes(&writer, data, SIZE_MAX, 0, &error) && !strcmp(error, "overflow"));
    CHECK(!mbp_write_array(&writer, values, SIZE_MAX / 4 + 1, &error) && !strcmp(error, "overflow"));
    CHECK(writer.position == 8);
    return 0;
}

static int unaligned(void)
{
    unsigned char storage[65];
    struct mbp_writer writer;
    struct mbp_reader reader;
    const char *error = NULL;
    size_t size;
    uint32_t value;
    int32_t signed_output;
    CHECK(mbp_writer_init(&writer, storage + 1, 64, &error));
    CHECK(mbp_write_u32(&writer, UINT32_C(0x12345678), &error));
    CHECK(mbp_write_i32(&writer, -7, &error));
    CHECK(mbp_writer_finish(&writer, &size, &error));
    CHECK(mbp_reader_init(&reader, storage + 1, size, &error));
    CHECK(mbp_read_u32(&reader, &value, &error) && value == UINT32_C(0x12345678));
    CHECK(mbp_read_i32(&reader, &signed_output, &error) && signed_output == -7);
    CHECK(mbp_reader_finish(&reader, &error));
    return 0;
}

static int finish(void)
{
    unsigned char wire[64];
    struct mbp_writer writer;
    struct mbp_reader reader;
    const char *error = "unchanged";
    size_t size = 99;
    uint32_t value;
    CHECK(mbp_writer_init(&writer, wire, sizeof(wire), NULL));
    CHECK(mbp_writer_finish(&writer, &size, &error) && size == 8);
    CHECK(!strcmp(error, "unchanged"));
    CHECK(mbp_reader_init(&reader, wire, size, NULL));
    CHECK(mbp_reader_finish(&reader, NULL));
    CHECK(!mbp_read_u32(&reader, &value, NULL) && reader.position == 8);
    CHECK(mbp_write_u32(&writer, 123, NULL));
    CHECK(mbp_writer_finish(&writer, &size, NULL));
    CHECK(mbp_reader_init(&reader, wire, size, NULL));
    CHECK(!mbp_reader_finish(&reader, &error) && !strcmp(error, "trailing"));
    CHECK(reader.position == 8);
    CHECK(mbp_read_u32(&reader, &value, NULL) && value == 123);
    CHECK(mbp_reader_finish(&reader, NULL));
    return 0;
}

int main(int argc, char **argv)
{
    int result = 1;
    if (argc != 2)
        return 2;
    if (!strcmp(argv[1], "roundtrip")) result = roundtrip();
    else if (!strcmp(argv[1], "header")) result = header();
    else if (!strcmp(argv[1], "bytes")) result = bytes();
    else if (!strcmp(argv[1], "array_capacity")) result = array_capacity();
    else if (!strcmp(argv[1], "failure_cursors")) result = failure_cursors();
    else if (!strcmp(argv[1], "limits")) result = limits();
    else if (!strcmp(argv[1], "unaligned")) result = unaligned();
    else if (!strcmp(argv[1], "finish")) result = finish();
    if (!result) printf("PASS %s\n", argv[1]);
    return result;
}
'''


class ParcelLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-parcel-api-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "parcel_api"
        harness = pathlib.Path(cls.directory.name) / "parcel_api.c"
        harness.write_text(HARNESS, encoding="ascii")
        source = ROOT / "userspace/parcel.c"
        if not source.is_file():
            raise AssertionError(f"Missing parcel library: {source}")
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-I", str(ROOT / "userspace"), str(harness),
             str(source), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"PASS {case}\n")

    def test_scalar_bytes_array_roundtrip(self):
        self.run_case("roundtrip")

    def test_header_and_initialization_failures_are_atomic(self):
        self.run_case("header")

    def test_nullable_views_padding_and_outputs(self):
        self.run_case("bytes")

    def test_array_capacity_truncation_and_guards(self):
        self.run_case("array_capacity")

    def test_failed_fields_preserve_nonzero_cursor_and_outputs(self):
        self.run_case("failure_cursors")

    def test_payload_limits_and_size_t_overflow(self):
        self.run_case("limits")

    def test_unaligned_wire_base(self):
        self.run_case("unaligned")

    def test_finish_and_optional_error_pointer(self):
        self.run_case("finish")


if __name__ == "__main__":
    unittest.main()
