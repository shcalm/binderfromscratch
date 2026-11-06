import pathlib
import signal
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class MappingModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-mapping-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "mapping"
        source = ROOT / "examples/memory/mapping.c"
        if not source.is_file():
            raise AssertionError(f"Missing mapping experiment: {source}")
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", str(source), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True, timeout=30)

    def run_model(self, *args, expected=0):
        result = subprocess.run([str(self.binary), *args], capture_output=True,
                                text=True, timeout=8)
        self.assertEqual(result.returncode, expected, result.stderr)
        events = [dict(field.split("=", 1) for field in line.split())
                  for line in result.stdout.splitlines()]
        return result, events

    def test_distinct_views_and_page_boundaries(self):
        _, events = self.run_model()
        views = next(e for e in events if e["event"] == "mapped")
        self.assertEqual(views["parent_prot"], "rw")
        self.assertEqual(views["child_prot"], "r")
        self.assertEqual(views["guards"], "none")
        self.assertGreaterEqual(int(views["bytes"]), 4096)
        self.assertNotEqual(int(views["parent_address"], 16), 0)
        self.assertNotEqual(int(views["child_address"], 16), 0)
        reads = [e for e in events if e["event"] == "read_ack"]
        self.assertEqual([(e["round"], e["first"], e["last"])
                          for e in reads], [("1", "7", "70"),
                                           ("2", "42", "420")])

    def test_close_precedes_both_handshakes_and_mapping_still_works(self):
        _, events = self.run_model()
        close = next(e for e in events if e["event"] == "fd_closed")
        self.assertEqual(close, {"event": "fd_closed", "parent": "1",
                                 "child": "1"})
        for round_number in ("1", "2"):
            published = next(e for e in events if e["event"] == "published"
                             and e["round"] == round_number)
            ack = next(e for e in events if e["event"] == "read_ack"
                       and e["round"] == round_number)
            self.assertLess(events.index(close), events.index(published))
            self.assertLess(events.index(published), events.index(ack))

    def test_ro_write_and_guard_read_fault_are_waited_and_checked(self):
        _, events = self.run_model()
        faults = [e for e in events if e["event"] == "fault"]
        self.assertEqual([(e["kind"], int(e["signal"])) for e in faults],
                         [("ro_write", signal.SIGSEGV),
                          ("guard_read", signal.SIGSEGV)])
        preserved = next(e for e in events if e["event"] == "preserved")
        self.assertEqual(preserved["first"], "42")
        self.assertEqual(preserved["last"], "420")
        self.assertEqual(events[-1], {"event": "cleanup", "children_reaped": "2",
                                      "unmapped": "1", "result": "ok"})

    def test_abort_reaps_reader_waiting_for_command(self):
        result, events = self.run_model("--abort-after-map", expected=1)
        self.assertIn("requested abort after map", result.stderr)
        self.assertFalse(any(e["event"] == "read_ack" for e in events))
        self.assertEqual(events[-1], {"event": "cleanup", "children_reaped": "1",
                                      "unmapped": "1", "result": "failed"})

    def test_unknown_option_is_rejected_before_creating_resources(self):
        result, events = self.run_model("--unknown", expected=2)
        self.assertIn("usage:", result.stderr)
        self.assertEqual(events, [])


if __name__ == "__main__":
    unittest.main()
