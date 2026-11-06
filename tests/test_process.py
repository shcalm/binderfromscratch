import pathlib
import signal
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ProcessExperiments(unittest.TestCase):
    def run_example(self, name, *args):
        binary = ROOT / "build" / name
        self.assertTrue(binary.is_file(), f"Missing experiment: {binary}")
        return subprocess.run([str(binary), *args], capture_output=True,
                              text=True, timeout=5)

    def test_join_makes_worker_write_visible(self):
        result = self.run_example("thread_shared")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [dict(field.split("=", 1) for field in line.split())
                 for line in result.stdout.splitlines()]
        self.assertEqual([line["event"] for line in lines],
                         ["before_create", "worker_write", "after_join"])
        self.assertEqual([line["result"] for line in lines], ["-1", "3", "3"])
        self.assertEqual(len({line["address"] for line in lines}), 1)
        self.assertEqual(len({line["pid"] for line in lines}), 1)

    def test_exit_status_is_not_a_full_integer_return(self):
        result = self.run_example("fork_status", "--exit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("child_result=300", result.stdout)
        self.assertIn("event=exited exit_status=44 parent_result=-1", result.stdout)

    def test_signal_termination_is_reported_and_reaped(self):
        result = self.run_example("fork_status", "--signal")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"event=signaled signal={signal.SIGTERM} parent_result=-1",
                      result.stdout)
        self.assertNotIn("event=exited", result.stdout)

    def test_status_experiment_rejects_unknown_mode(self):
        result = self.run_example("fork_status", "--unknown")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("usage:", result.stderr)

    def test_fork_failure_uses_process_local_resource_limit(self):
        result = self.run_example("fork_failure")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("event=fork_rejected errno=11 child_created=0", result.stdout)


if __name__ == "__main__":
    unittest.main()
