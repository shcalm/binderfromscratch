import pathlib
import random
import struct
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAGIC = b"MBP1"
LIMIT = 4096


def oracle(sequence, status, label, values):
    wire = MAGIC + struct.pack("<IIi", 1, sequence, status)
    if label is None:
        wire += struct.pack("<I", 0xFFFFFFFF)
    else:
        wire += struct.pack("<I", len(label)) + label
        wire += b"\0" * (-len(label) % 4)
    return wire + struct.pack("<I", len(values)) + b"".join(
        struct.pack("<i", value) for value in values)


class ParcelModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-parcel-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "parcel"
        source = ROOT / "examples/models/parcel.c"
        if not source.is_file():
            raise AssertionError(f"Missing parcel model: {source}")
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-I", str(ROOT / "userspace"), str(source),
             str(ROOT / "userspace/parcel.c"), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True, timeout=30)

    def invoke(self, *arguments, expected=0):
        result = subprocess.run([str(self.binary), *arguments], text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def decoded(self, sequence, status, label, values):
        return {"sequence": str(sequence), "status": str(status),
                "label": "null" if label is None else label.hex(),
                "values": ",".join(map(str, values))}

    def parse(self, output):
        return dict(line.split("=", 1) for line in output.splitlines())

    def reject(self, wire, reason=None):
        result = self.invoke("decode", wire.hex(), expected=1)
        self.assertEqual(result.stdout, "")
        self.assertIn("error=", result.stderr)
        if reason:
            self.assertIn("error=" + reason, result.stderr)

    def test_demo_matches_independent_wire_oracle(self):
        fields = (0x12345678, -7, b"A\0B", [1, -2, 2147483647, -2147483648])
        output = self.parse(self.invoke("demo").stdout)
        self.assertEqual(output.pop("hex"), oracle(*fields).hex())
        self.assertEqual(output, self.decoded(*fields))

    def test_raw_struct_copies_pointer_values_and_keeps_aliases(self):
        self.assertEqual(self.parse(self.invoke("raw").stdout),
                         {"same_label_pointer": "1", "same_array_pointer": "1",
                          "raw_label_before": "410042", "raw_label_after": "5a0042",
                          "raw_array_after": "99,-2", "wire_label": "410042",
                          "wire_values": "1,-2"})

    def test_encode_and_decode_against_independent_oracle(self):
        cases = [(0, 0, None, []), (0xFFFFFFFF, -2147483648, b"", []),
                 (1, 2147483647, b"\xff\x00\x80", [-1, 0, 1])]
        rng = random.Random(20261106)
        for _ in range(24):
            cases.append((rng.randrange(2**32), rng.randrange(-2**31, 2**31),
                          bytes(rng.randrange(256) for _ in range(rng.randrange(17))),
                          [rng.randrange(-2**31, 2**31) for _ in range(rng.randrange(9))]))
        for sequence, status, label, values in cases:
            with self.subTest(sequence=sequence, label=label, values=values):
                wire = oracle(sequence, status, label, values)
                encoded = self.parse(self.invoke(
                    "encode", str(sequence), str(status),
                    "null" if label is None else label.hex(),
                    ",".join(map(str, values))).stdout)
                self.assertEqual(encoded, {"hex": wire.hex()})
                output = self.parse(self.invoke("decode", wire.hex()).stdout)
                self.assertEqual(output, self.decoded(sequence, status, label, values))

    def test_null_empty_embedded_nul_and_non_utf8_remain_distinct(self):
        for label in (None, b"", b"\0", b"A\0B", b"\xff\xfe"):
            with self.subTest(label=label):
                wire = oracle(1, -1, label, [])
                self.assertEqual(self.parse(self.invoke("decode", wire.hex()).stdout),
                                 self.decoded(1, -1, label, []))
        self.assertNotEqual(oracle(1, 0, None, []), oracle(1, 0, b"", []))

    def test_every_proper_prefix_is_rejected(self):
        wire = oracle(0x12345678, -7, b"A\0B", [1, -2, 2147483647, -2147483648])
        for length in range(len(wire)):
            with self.subTest(length=length):
                self.reject(wire[:length])

    def test_bad_header_padding_lengths_counts_and_trailing_bytes(self):
        wire = oracle(7, -3, b"abc", [1, 2])
        self.reject(b"NOPE" + wire[4:], "magic")
        for version in (0, 2, 0xFFFFFFFF):
            self.reject(wire[:4] + struct.pack("<I", version) + wire[8:], "version")
        self.reject(wire[:23] + b"\x01" + wire[24:], "padding")
        for length in (4097, 0x40000000, 0xFFFFFFFE):
            self.reject(wire[:16] + struct.pack("<I", length) + wire[20:])
        for count in (1025, 0x40000000, 0xFFFFFFFF):
            self.reject(wire[:24] + struct.pack("<I", count) + wire[28:])
        self.reject(wire + b"\0", "trailing")
        self.reject(wire + b"\0" * 4, "trailing")
        self.reject(wire + b"\0" * LIMIT, "limit")

    def test_exact_payload_limit_for_bytes_and_arrays(self):
        for label, values in ((b"x" * 4072, []), (None, [1] * 1018)):
            wire = oracle(1, -1, label, values)
            self.assertEqual(len(wire), LIMIT)
            encoded = self.parse(self.invoke("encode", "1", "-1",
                                             "null" if label is None else label.hex(),
                                             ",".join(map(str, values))).stdout)
            self.assertEqual(encoded["hex"], wire.hex())
            self.assertEqual(self.parse(self.invoke("decode", wire.hex()).stdout),
                             self.decoded(1, -1, label, values))
        for label, values in ((b"x" * 4073, []), (None, [1] * 1019)):
            result = self.invoke("encode", "1", "-1",
                                 "null" if label is None else label.hex(),
                                 ",".join(map(str, values)), expected=1)
            self.assertEqual(result.stdout, "")
            self.assertIn("error=limit", result.stderr)

    def test_failed_primitive_and_message_cursors_do_not_advance(self):
        self.assertEqual(self.parse(self.invoke("selftest").stdout),
                         {"cursor_checks": "10", "overflow_checks": "4"})

    def test_invalid_cli_values_and_hex_are_rejected_without_output(self):
        for value in ("0", "gg", "0x00", "00 00"):
            result = self.invoke("decode", value, expected=1)
            self.assertEqual(result.stdout, "")
            self.assertIn("error=hex", result.stderr)
        for arguments in (("4294967296", "0", "", ""),
                          ("-1", "0", "", ""),
                          ("0", "2147483648", "", ""),
                          ("0", "-2147483649", "", ""),
                          ("0", "0", "", "1,"),
                          ("0", "0", "", "1,,2"),
                          ("0", "0", "", "2147483648")):
            self.assertEqual(self.invoke("encode", *arguments, expected=1).stdout, "")
        self.assertIn("usage:", self.invoke("unknown", expected=2).stderr)


if __name__ == "__main__":
    unittest.main()
