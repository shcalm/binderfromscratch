import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class QueueModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-model-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "request_queue"
        source = ROOT / "examples/models/request_queue.c"
        if not source.is_file():
            raise AssertionError(f"Missing queue model: {source}")
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", str(source), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True, timeout=30)

    def run_model(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [dict(field.split("=", 1) for field in line.split())
                for line in result.stdout.splitlines()]

    def test_overwrite_loses_first_clients_request(self):
        events = self.run_model("overwrite")
        lost = [event for event in events if event["event"] == "overwritten"]
        self.assertEqual(lost, [{"event": "overwritten", "request_id": "101",
                                 "client": "1"}])
        results = [event for event in events if event["event"] == "client_result"]
        self.assertEqual(results, [{"event": "client_result", "client": "2",
                                    "request_id": "102", "result": "42"}])

    def test_refusal_preserves_first_request_and_requires_retry(self):
        events = self.run_model("refuse")
        refused = [event for event in events if event["event"] == "refused"]
        self.assertEqual(refused, [{"event": "refused", "request_id": "102",
                                    "reason": "slot_full"}])
        self.assertLess(events.index(refused[0]),
                        next(i for i, event in enumerate(events)
                             if event["event"] == "retry"))
        results = [event for event in events if event["event"] == "client_result"]
        self.assertEqual([(event["client"], event["request_id"], event["result"])
                          for event in results], [("1", "101", "3"),
                                                 ("2", "102", "42")])

    def test_fifo_delivery_keeps_results_associated_when_replies_reverse(self):
        events = self.run_model("fifo")
        delivered = [event["request_id"] for event in events
                     if event["event"] == "delivered"]
        self.assertEqual(delivered, ["101", "102"])
        results = [event for event in events if event["event"] == "client_result"]
        self.assertEqual([(event["client"], event["request_id"], event["result"])
                          for event in results], [("2", "102", "42"),
                                                 ("1", "101", "3")])

    def test_fifo_rejects_full_queue_and_invalid_reply_ids(self):
        events = self.run_model("fifo")
        refused = [event for event in events if event["event"] == "refused"]
        self.assertEqual(refused, [{"event": "refused", "request_id": "103",
                                    "reason": "queue_full"}])
        rejected = [event for event in events if event["event"] == "reply_rejected"]
        self.assertEqual(rejected, [
            {"event": "reply_rejected", "request_id": "999", "reason": "unknown"},
            {"event": "reply_rejected", "request_id": "101", "reason": "not_delivered"},
            {"event": "reply_rejected", "request_id": "102", "reason": "duplicate"},
        ])

    def test_unknown_mode_is_rejected(self):
        result = subprocess.run([str(self.binary), "bogus"], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
