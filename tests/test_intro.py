import pathlib
import os
import select
import signal
import subprocess
import sys
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_ready_line(stream, timeout):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while b"\n" not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("incomplete readiness line")
        ready, _, _ = select.select([stream], [], [], remaining)
        if not ready:
            raise TimeoutError("incomplete readiness line")
        chunk = os.read(stream.fileno(), 4096)
        if not chunk:
            raise EOFError("server closed stdout before readiness line")
        data.extend(chunk)
        if len(data) > 4096:
            raise ValueError("readiness line exceeds 4096 bytes")
    return data.split(b"\n", 1)[0].decode("ascii")


class IntroExperiments(unittest.TestCase):
    def run_example(self, name, *args):
        binary = ROOT / "build" / name
        self.assertTrue(binary.is_file(), f"Missing experiment: {binary}")
        return subprocess.run([str(binary), *args], capture_output=True,
                              text=True, timeout=5)

    def test_local_call(self):
        result = self.run_example("local_add")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("result=3", result.stdout)

    def test_fork_does_not_share_result(self):
        result = self.run_example("fork_private")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("child result=3", result.stdout)
        self.assertIn("parent result=-1", result.stdout)

    def test_rpc_executes_in_another_process(self):
        result = self.run_example("socket_rpc", "1", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = dict(item.split("=", 1) for item in result.stdout.split())
        self.assertEqual(fields["result"], "3")
        self.assertNotEqual(fields["client_pid"], fields["server_pid"])

    def test_rpc_other_arguments(self):
        for a, b, expected in [("0", "0", "0"), ("19", "23", "42"),
                               ("4294967295", "0", "4294967295")]:
            with self.subTest(a=a, b=b):
                result = self.run_example("socket_rpc", a, b)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"result={expected}", result.stdout)

    def test_overflow_is_a_remote_error(self):
        result = self.run_example("socket_rpc", "4294967295", "1")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("remote error: overflow", result.stderr)

    def test_invalid_arguments(self):
        for value in ["-1", "4294967296", "abc", "", "1x"]:
            with self.subTest(value=value):
                result = self.run_example("socket_rpc", value, "2")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("invalid argument", result.stderr)

    def test_server_exit_does_not_leave_client_waiting(self):
        result = self.run_example("socket_rpc", "1", "2", "--server-exit")
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertIn("transport error", result.stderr)

    def test_malformed_request_lengths_are_rejected(self):
        for option in ["--short-request", "--long-request"]:
            with self.subTest(option=option):
                result = self.run_example("socket_rpc", "1", "2", option)
                self.assertEqual(result.returncode, 3, result.stderr)
                self.assertIn("server protocol error: malformed request", result.stderr)
                self.assertIn("transport error", result.stderr)
                self.assertNotIn("result=", result.stdout)

    def test_unknown_operation_is_a_remote_error(self):
        result = self.run_example("socket_rpc", "1", "2", "--unknown-op")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("remote error: unknown operation", result.stderr)
        self.assertNotIn("transport error", result.stderr)

    def test_live_server_without_reply_needs_harness_deadline(self):
        process = subprocess.Popen(
            [str(ROOT / "build/socket_rpc"), "1", "2", "--server-no-reply"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            start_new_session=True)
        try:
            ready = read_ready_line(process.stdout, timeout=5)
            self.assertIn("event=request_received_no_reply", ready)
            fields = dict(item.split("=", 1) for item in ready.split())
            server_pid = int(fields["server_pid"])
            os.kill(server_pid, 0)
            self.assertNotRegex(pathlib.Path(f"/proc/{server_pid}/status").read_text(),
                                r"(?m)^State:\s+[ZX]")
            with self.assertRaises(subprocess.TimeoutExpired):
                process.communicate(timeout=0.2)
            self.assertIsNone(process.poll())
            os.kill(server_pid, 0)
            self.assertNotRegex(pathlib.Path(f"/proc/{server_pid}/status").read_text(),
                                r"(?m)^State:\s+[ZX]")
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 124, stderr)
            self.assertIn("harness interruption: no protocol timeout", stderr)
            self.assertNotIn("transport error", stderr)
            self.assertNotIn("result=", stdout)
            with self.assertRaises(ProcessLookupError):
                os.kill(server_pid, 0)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.communicate(timeout=5)
            process.stdout.close()
            process.stderr.close()

    def test_partial_readiness_line_times_out_and_cleans_up(self):
        self.assertTrue(callable(globals().get("read_ready_line")),
                        "missing bounded readiness reader")
        producer = subprocess.Popen(
            [sys.executable, "-c",
             "import os, signal; os.write(1, b'event=partial'); "
             "os.write(2, b'R'); signal.pause()"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reader = None
        try:
            ready, _, _ = select.select([producer.stderr], [], [], 5)
            self.assertTrue(ready, "partial-output producer did not start")
            self.assertEqual(os.read(producer.stderr.fileno(), 1), b"R")
            reader = subprocess.Popen(
                [sys.executable, "-c",
                 "import sys\nfrom test_intro import read_ready_line\n"
                 "try:\n    read_ready_line(sys.stdin, 0.1)\n"
                 "except TimeoutError:\n    print('event=readiness_timeout')\n"],
                cwd=ROOT / "tests", stdin=producer.stdout,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            stdout, stderr = reader.communicate(timeout=2)
            self.assertEqual(reader.returncode, 0, stderr)
            self.assertIn("event=readiness_timeout", stdout)
            self.assertIsNone(producer.poll())
        finally:
            for process in [reader, producer]:
                if process is None:
                    continue
                if process.poll() is None:
                    process.terminate()
                try:
                    process.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=2)
        self.assertIsNotNone(producer.returncode)
        self.assertIsNotNone(reader.returncode)


if __name__ == "__main__":
    unittest.main()
