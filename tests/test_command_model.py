import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class CommandModel(unittest.TestCase):
    def test_invalid_commands_never_modify_the_result_buffer(self):
        source = ROOT / "examples/models/command_dispatch.c"
        self.assertTrue(source.is_file(), "Missing executable command model")
        with tempfile.TemporaryDirectory(prefix="mini-binder-command-") as directory:
            binary = pathlib.Path(directory) / "command_dispatch"
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", str(source), "-o", str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            "case=valid status=OK version=1 bytes=24",
            "case=unknown_number status=ENOTTY unchanged=1",
            "case=wrong_encoded_size status=ENOTTY unchanged=1",
            "case=wrong_direction status=ENOTTY unchanged=1",
            "case=short_local_buffer status=EFAULT unchanged=1",
            "COMMAND_MODEL_PASS",
        ])
