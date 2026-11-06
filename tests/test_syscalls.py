import errno
import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class SyscallExperiments(unittest.TestCase):
    def run_example(self, name, *arguments):
        binary = ROOT / "build" / name
        self.assertTrue(binary.is_file(), f"Missing experiment: {binary}")
        return subprocess.run([str(binary), *arguments], text=True,
                              capture_output=True, timeout=5)

    def test_successful_write_does_not_make_stale_errno_an_error(self):
        result = self.run_example("errno_write")
        self.assertEqual(result.returncode, 0, result.stderr)
        events = {line.split()[0]: line for line in result.stdout.splitlines()}
        self.assertIn(f"return=1 errno={errno.EDOM} decision=success",
                      events["event=write_success"])
        self.assertIn("return=1 value=X", events["event=payload_read"])

    def test_failed_write_captures_errno_only_after_negative_return(self):
        result = self.run_example("errno_write")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"event=write_failure return=-1 errno={errno.EBADF} decision=stop",
                      result.stdout)

    def test_interrupted_wait_can_retry_then_read_after_acknowledgment(self):
        result = self.run_example("interrupt_wait", "--retry")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 3, result.stdout)
        self.assertIn("event=waiting", lines[0])
        self.assertIn(f"event=interrupted return=-1 errno={errno.EINTR} signal_seen=1 action=retry",
                      lines[1])
        self.assertIn("event=read_complete return=1 value=X interruptions=1", lines[2])

    def test_interruption_can_mean_cancel_instead_of_blind_retry(self):
        result = self.run_example("interrupt_wait", "--stop")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"event=interrupted return=-1 errno={errno.EINTR} signal_seen=1 action=stop",
                      result.stdout)
        self.assertIn("event=cancelled interruptions=1", result.stdout)
        self.assertNotIn("event=read_complete", result.stdout)

    def test_unknown_wait_policy_does_not_create_a_child(self):
        result = self.run_example("interrupt_wait", "--unknown")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("usage:", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
