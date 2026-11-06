import itertools
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
EVENTS = ("deliver", "receive_fault", "reply", "reply_empty", "reply_fault",
          "cancel", "server_exit", "wrong_reply", "output_ok", "output_fault",
          "free_request", "free_reply", "reap")

# Contract oracle for canonical states: queued request, delivered request,
# accepted reply pending output, and canceled delivered request, respectively.
ALLOWED = {
    "QUEUED": {"deliver": "DELIVERED", "receive_fault": "QUEUED",
               "cancel": "FAILED", "server_exit": "FAILED"},
    "DELIVERED": {"reply": "REPLIED", "reply_empty": "REPLIED",
                  "reply_fault": "DELIVERED", "cancel": "FAILED",
                  "server_exit": "FAILED", "free_request": "DELIVERED"},
    "REPLIED": {"server_exit": "REPLIED", "output_ok": "REPLIED",
                "output_fault": "REPLIED", "free_request": "REPLIED",
                "reap": "REPLIED"},
    "FAILED": {"server_exit": "FAILED", "free_request": "FAILED", "reap": "FAILED"},
}


class TransactionModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-transaction-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "transactions"
        source = ROOT / "examples/models/transactions.c"
        if not source.is_file():
            raise AssertionError(f"Missing transaction model: {source}")
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", str(source), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True, timeout=30)

    def run_model(self, *arguments, expected=0):
        result = subprocess.run([str(self.binary), *arguments], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        events = [dict(field.split("=", 1) for field in line.split())
                  for line in result.stdout.splitlines()]
        return result, events

    def trace(self, *events):
        _, output = self.run_model("trace", *events)
        steps = [event for event in output if event["event"] == "step"]
        self.assertEqual([event["operation"] for event in steps], list(events))
        for step in steps:
            self.assert_ledger(step)
        return steps

    def assert_ledger(self, event):
        terminal = event["state"] in ("REPLIED", "FAILED")
        self.assertEqual(int(event["terminal_count"]), int(terminal))
        self.assertEqual(int(event["request_frees"]), int(event["request"] == "NONE"))
        self.assertEqual(int(event["reply_allocs"]) - int(event["reply_frees"]),
                         int(event["reply"] != "NONE"))
        self.assertEqual(int(event["record_reaps"]), 1 - int(event["record_live"]))

    def test_single_shared_slot_loses_a_result_when_completion_reverses(self):
        _, events = self.run_model("slot")
        writes = [event for event in events if event["event"] == "slot_write"]
        reads = [event for event in events if event["event"] == "slot_read"]
        self.assertEqual([(e["logical_call"], e["result"]) for e in writes],
                         [("2", "42"), ("1", "3")])
        self.assertEqual([(e["logical_call"], e["expected"], e["observed"])
                          for e in reads], [("1", "3", "3"), ("2", "42", "3")])
        self.assertEqual(writes[1]["overwritten_call"], "2")

    def test_unique_transaction_ids_bind_reverse_results_despite_repeated_sequence(self):
        _, events = self.run_model("reorder")
        results = [event for event in events if event["event"] == "client_result"]
        self.assertEqual([(e["id"], e["logical_call"], e["result"])
                          for e in results], [("102", "2", "42"), ("101", "1", "3")])
        self.assertEqual({e["client"] for e in results}, {"1"})
        self.assertEqual({e["sequence"] for e in results}, {"7"})
        rejected = [e for e in events if e["event"] == "rejected"]
        self.assertEqual(rejected, [{"event": "rejected", "id": "999", "reason": "unknown"}])
        steps = [e for e in events if e["event"] == "step"]
        self.assertTrue(any(e["operation"] == "reply" and e["reason"] == "not_delivered"
                            for e in steps))
        self.assertTrue(any(e["operation"] == "wrong_reply" and e["accepted"] == "0"
                            for e in steps))
        self.assertTrue(any(e["operation"] == "reply" and e["reason"] == "terminal"
                            for e in steps))
        for step in steps:
            self.assert_ledger(step)
        self.assertEqual(events[-1], {"event": "summary", "records": "0",
                                      "request_slots": "0", "reply_slots": "0",
                                      "terminal_total": "2"})

    def test_full_state_event_matrix_uses_contract_oracle(self):
        _, events = self.run_model("matrix")
        self.assertEqual(len(events), 4 * len(EVENTS))
        self.assertEqual({(e["from"], e["operation"]) for e in events},
                         set(itertools.product(ALLOWED, EVENTS)))
        for event in events:
            with self.subTest(state=event["from"], operation=event["operation"]):
                state, operation = event["from"], event["operation"]
                expected_state = ALLOWED[state].get(operation, state)
                self.assertEqual(event["state"], expected_state)
                self.assertEqual(int(event["accepted"]), int(operation in ALLOWED[state]))
                self.assert_ledger(event)
                if state == "QUEUED":
                    request = {"deliver": "SERVER", "cancel": "NONE",
                               "server_exit": "NONE"}.get(operation, "DRIVER")
                else:
                    request = "NONE" if operation in ("server_exit", "free_request") else "SERVER"
                self.assertEqual(event["request"], request)
                if state == "REPLIED":
                    reply = {"output_ok": "CLIENT", "output_fault": "NONE"}.get(operation, "PREPARED")
                else:
                    reply = "PREPARED" if state == "DELIVERED" and operation == "reply" else "NONE"
                self.assertEqual(event["reply"], reply)

    def test_all_pairs_of_transaction_events_respect_terminal_monotonicity(self):
        core = EVENTS[:8]
        for pair in itertools.product(core, repeat=2):
            with self.subTest(events=pair):
                steps = self.trace("deliver", *pair)
                state = "QUEUED"
                for event in steps:
                    operation = event["operation"]
                    state = ALLOWED[state].get(operation, state)
                    self.assertEqual(event["state"], state)
                self.assertLessEqual(int(steps[-1]["terminal_count"]), 1)

    def test_cancel_and_reply_have_two_observable_winners(self):
        reply_first = self.trace("deliver", "reply", "cancel", "reap", "output_ok")
        cancel_first = self.trace("deliver", "cancel", "reply", "output_ok")
        self.assertEqual(reply_first[-1]["state"], "REPLIED")
        self.assertEqual(reply_first[2]["accepted"], "0")
        self.assertEqual(reply_first[-1]["reply"], "CLIENT")
        self.assertEqual(cancel_first[-1]["state"], "FAILED")
        self.assertEqual(cancel_first[2]["accepted"], "0")
        self.assertEqual(cancel_first[-1]["reply_allocs"], "0")

    def test_receive_and_reply_faults_rollback_and_allow_retry(self):
        steps = self.trace("receive_fault", "deliver", "reply_fault", "reply")
        self.assertEqual((steps[0]["state"], steps[0]["request"]), ("QUEUED", "DRIVER"))
        self.assertEqual((steps[2]["state"], steps[2]["reply"], steps[2]["terminal_count"]),
                         ("DELIVERED", "NONE", "0"))
        self.assertEqual((steps[2]["reply_allocs"], steps[2]["reply_frees"]), ("1", "1"))
        self.assertEqual((steps[3]["state"], steps[3]["reply_allocs"], steps[3]["reply_frees"]),
                         ("REPLIED", "2", "1"))

    def test_request_reply_and_record_reclamation_are_independent(self):
        steps = self.trace("deliver", "free_request", "reply", "reap", "output_ok", "free_reply")
        self.assertEqual((steps[1]["state"], steps[1]["record_live"]), ("DELIVERED", "1"))
        self.assertEqual((steps[3]["record_live"], steps[3]["reply"]), ("0", "PREPARED"))
        self.assertEqual((steps[-1]["state"], steps[-1]["reply"], steps[-1]["record_live"]),
                         ("REPLIED", "NONE", "0"))
        self.assertEqual(self.trace("cancel")[-1]["request"], "NONE")
        self.assertEqual(self.trace("deliver", "cancel")[-1]["request"], "SERVER")

    def test_output_efault_rolls_back_reply_slot_without_second_termination(self):
        steps = self.trace("deliver", "reply", "reap", "output_fault", "free_request")
        self.assertEqual((steps[3]["state"], steps[3]["output"], steps[3]["reply"]),
                         ("REPLIED", "FAULT", "NONE"))
        self.assertEqual((steps[3]["reply_allocs"], steps[3]["reply_frees"]), ("1", "1"))
        self.assertEqual(steps[-1]["terminal_count"], "1")
        empty = self.trace("deliver", "reply_empty", "reap", "output_ok", "free_reply")
        self.assertEqual(empty[-1]["accepted"], "0")
        self.assertEqual(empty[-1]["reply_allocs"], "0")
        _, output = self.run_model("trace", "deliver", "reply_empty", "output_ok")
        self.assertEqual(next(e for e in output if e["event"] == "client_result")["result"], "none")

    def test_server_exit_cleans_requests_without_overriding_accepted_reply(self):
        steps = self.trace("deliver", "reply", "server_exit", "reap", "output_ok", "free_reply")
        self.assertEqual((steps[2]["state"], steps[2]["request"], steps[2]["reply"]),
                         ("REPLIED", "NONE", "PREPARED"))
        failed = self.trace("deliver", "server_exit", "reply", "reap")
        self.assertEqual((failed[-1]["state"], failed[-1]["request"]), ("FAILED", "NONE"))

    def test_repeated_free_output_reap_and_terminal_backtracking_are_rejected(self):
        steps = self.trace("deliver", "reply", "output_ok", "output_fault", "free_reply",
                           "free_reply", "reap", "reap", "cancel", "deliver", "reply")
        self.assertEqual([steps[i]["accepted"] for i in (3, 5, 7, 8, 9, 10)], ["0"] * 6)
        self.assertTrue(all(e["state"] == "REPLIED" for e in steps[1:]))
        self.assertEqual(steps[-1]["record_reaps"], "1")

    def test_cli_rejects_unknown_modes_and_events_before_output(self):
        for arguments in ((), ("unknown",), ("trace",), ("trace", "deliver", "unknown")):
            result, events = self.run_model(*arguments, expected=2)
            self.assertEqual(events, [])
            self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
