import itertools
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class NodeModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-node-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "nodes"
        source = ROOT / "examples/models/nodes.c"
        if not source.is_file():
            raise AssertionError(f"Missing node model: {source}")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pedantic", str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True, timeout=30)

    def run_model(self, *arguments, expected=0):
        result = subprocess.run([str(self.binary), *arguments], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        rows = [dict(field.split("=", 1) for field in line.split())
                for line in result.stdout.splitlines()]
        return result, rows

    def trace(self, *events):
        _, rows = self.run_model("trace", *events)
        steps = [row for row in rows if row["event"] == "step"]
        self.assertEqual([row["operation"] for row in steps], list(events))
        for row in steps:
            self.assert_ledger(row)
        return steps

    def assert_ledger(self, row):
        refs = sum(int(row[field]) for field in ("published", "tx_refs", "slot_refs"))
        self.assertEqual(int(row["refs"]), refs)
        self.assertEqual(int(row["live_nodes"]), int(refs != 0))
        self.assertEqual(int(row["node_allocs"]) - int(row["node_frees"]), int(refs != 0))
        self.assertEqual(row["tx_refs"], row["tx_live"])
        self.assertEqual(int(row["slot_refs"]), int(row["request"] != "NONE"))
        self.assertEqual(int(row["tx_acquires"]) - int(row["tx_releases"]), int(row["tx_refs"]))
        self.assertEqual(int(row["slot_acquires"]) - int(row["slot_releases"]), int(row["slot_refs"]))
        terminal = row["state"] in ("REPLIED", "FAILED")
        self.assertEqual(int(row["terminal_count"]), int(terminal))
        self.assertEqual(int(row["accepted"]), int(row["status"] == "OK"))
        if row["owner_alive"] == "0":
            self.assertEqual((row["published"], row["slot_refs"], row["local_alive"]),
                             ("0", "0", "0"))

    def test_method_number_alone_merges_two_counter_states(self):
        _, rows = self.run_model("method")
        calls = [row for row in rows if row["event"] == "method_call"]
        self.assertEqual([(r["object"], r["code"], r["expected"], r["observed"])
                          for r in calls],
                         [("A", "1", "1", "1"), ("B", "1", "1", "2"),
                          ("A", "1", "2", "3")])

    def test_registry_dispatches_same_method_to_independent_persistent_objects(self):
        _, rows = self.run_model("registry")
        calls = [r for r in rows if r["event"] == "result"]
        self.assertEqual([(r["node"], r["code"], r["status"], r["value"])
                          for r in calls],
                         [("101", "1", "OK", "1"), ("102", "1", "OK", "101"),
                          ("101", "1", "OK", "3"), ("102", "2", "OK", "101"),
                          ("103", "3", "OK", "42"), ("101", "999", "ENOSYS", "none")])
        published = [r for r in rows if r["event"] == "publish"]
        self.assertEqual([(r["cookie"], r["node"], r["created"]) for r in published],
                         [("11", "101", "1"), ("22", "102", "1"),
                          ("33", "103", "1"), ("11", "101", "0")])
        self.assertEqual(rows[-1], {"event": "summary", "live_nodes": "0",
                                    "published": "0", "local_active": "0"})

    def test_duplicate_publish_is_idempotent_and_wrong_owner_cannot_remove(self):
        steps = self.trace("publish", "publish", "wrong_remove", "remove", "remove")
        self.assertEqual([r["status"] for r in steps], ["OK", "OK", "EPERM", "OK", "ENOENT"])
        self.assertTrue(all(r["node"] == "101" for r in steps))
        self.assertEqual(steps[1]["node_allocs"], "1")
        self.assertEqual(steps[-1]["refs"], "0")

    def test_transaction_and_request_slot_each_independently_block_remove(self):
        tx = self.trace("call", "deliver", "free_request", "remove", "reply", "remove",
                        "reap", "remove")
        self.assertEqual((tx[3]["status"], tx[3]["tx_refs"], tx[3]["slot_refs"]),
                         ("EBUSY", "1", "0"))
        self.assertEqual(tx[5]["status"], "EBUSY")
        self.assertEqual(tx[-1]["status"], "OK")
        slot = self.trace("call", "deliver", "reply", "reap", "remove", "free_request", "remove")
        self.assertEqual((slot[4]["status"], slot[4]["tx_refs"], slot[4]["slot_refs"]),
                         ("EBUSY", "0", "1"))
        self.assertEqual(slot[-1]["refs"], "0")

    def test_busy_remove_keeps_node_published_and_callable(self):
        steps = self.trace("call", "remove", "cancel", "reap", "call")
        self.assertEqual((steps[1]["status"], steps[1]["published"]), ("EBUSY", "1"))
        self.assertEqual((steps[-1]["status"], steps[-1]["state"]), ("OK", "QUEUED"))

    def test_queued_and_delivered_cancellation_have_different_slot_responsibilities(self):
        queued = self.trace("call", "cancel", "remove", "reap", "remove")
        delivered = self.trace("call", "deliver", "cancel", "reap", "remove", "free_request", "remove")
        self.assertEqual((queued[1]["state"], queued[1]["slot_refs"]), ("FAILED", "0"))
        self.assertEqual(queued[2]["status"], "EBUSY")
        self.assertEqual(queued[-1]["status"], "OK")
        self.assertEqual((delivered[4]["status"], delivered[4]["slot_refs"]), ("EBUSY", "1"))
        self.assertEqual(delivered[-1]["refs"], "0")

    def test_cookie_reuse_gets_new_id_and_rejects_old_target_and_descriptor(self):
        steps = self.trace("call", "deliver", "free_request", "reply", "reap", "remove",
                           "destroy", "publish", "call_stale", "local_begin")
        self.assertEqual((steps[7]["node"], steps[7]["cookie"], steps[7]["created"]),
                         ("102", "7", "1"))
        self.assertEqual((steps[8]["status"], steps[9]["status"]), ("ENOENT", "ESTALE"))
        self.assertEqual(steps[9]["descriptor_node"], "101")

    def test_local_activity_survives_kernel_release_and_prevents_local_destroy(self):
        steps = self.trace("call", "deliver", "local_begin", "free_request", "cancel",
                           "reap", "remove", "destroy", "publish", "local_end", "destroy", "publish")
        self.assertEqual((steps[6]["status"], steps[6]["refs"], steps[6]["local_active"]),
                         ("OK", "0", "1"))
        self.assertEqual((steps[7]["status"], steps[8]["status"]), ("EBUSY", "EBUSY"))
        self.assertEqual((steps[10]["status"], steps[11]["node"]), ("OK", "102"))

    def test_publication_fault_rolls_back_only_new_node_and_does_not_reuse_id(self):
        steps = self.trace("publish_fault", "remove", "destroy", "publish_fault", "publish")
        self.assertEqual((steps[0]["status"], steps[0]["node_allocs"], steps[0]["published"]),
                         ("EFAULT", "1", "1"))
        self.assertEqual((steps[3]["status"], steps[3]["live_nodes"], steps[3]["node_allocs"],
                          steps[3]["node_frees"]), ("EFAULT", "0", "2", "2"))
        self.assertEqual(steps[4]["node"], "103")

    def test_failed_request_publish_and_receive_output_preserve_reference_ledger(self):
        steps = self.trace("call_fault", "call", "receive_fault", "remove", "deliver")
        self.assertEqual((steps[0]["status"], steps[0]["refs"]), ("EFAULT", "1"))
        self.assertEqual((steps[2]["status"], steps[2]["state"], steps[2]["slot_refs"]),
                         ("EFAULT", "QUEUED", "1"))
        self.assertEqual(steps[3]["status"], "EBUSY")
        self.assertEqual(steps[-1]["state"], "DELIVERED")

    def test_owner_exit_preserves_only_orphan_transaction_reference_until_reap(self):
        for prefix in (("call",), ("call", "deliver"), ("call", "deliver", "reply")):
            with self.subTest(prefix=prefix):
                rows = self.trace(*prefix, "owner_exit", "call", "publish", "reap")
                exited = rows[len(prefix)]
                self.assertEqual((exited["owner_alive"], exited["published"], exited["tx_refs"],
                                  exited["slot_refs"], exited["live_nodes"]),
                                 ("0", "0", "1", "0", "1"))
                self.assertEqual(exited["state"], "REPLIED" if prefix[-1] == "reply" else "FAILED")
                self.assertEqual((rows[-3]["status"], rows[-2]["status"]), ("ENOENT", "EPIPE"))
                self.assertEqual(rows[-1]["live_nodes"], "0")

    def test_event_pairs_preserve_counts_and_terminal_monotonicity(self):
        events = ("deliver", "reply", "cancel", "reap", "free_request", "remove", "owner_exit")
        for pair in itertools.product(events, repeat=2):
            with self.subTest(pair=pair):
                rows = self.trace("call", *pair)
                terminal = None
                for row in rows:
                    if terminal:
                        self.assertEqual(row["state"], terminal)
                    elif row["state"] in ("REPLIED", "FAILED"):
                        terminal = row["state"]

    def test_local_dispatch_requires_a_delivered_matching_descriptor_and_one_activity(self):
        rows = self.trace("local_begin", "call", "deliver", "local_begin", "local_begin",
                          "local_end", "local_end")
        self.assertEqual([r["status"] for r in rows],
                         ["ESTALE", "OK", "OK", "OK", "EBUSY", "OK", "ENOENT"])
        self.assertEqual(rows[4]["local_active"], "1")
        self.assertEqual(rows[-1]["local_active"], "0")

    def test_repeated_releases_and_terminal_events_do_not_change_reference_counts(self):
        rows = self.trace("call", "free_request", "deliver", "free_request", "free_request",
                          "reply", "cancel", "reply", "reap", "reap", "remove", "remove")
        self.assertEqual([rows[i]["status"] for i in (1, 4, 6, 7, 9, 11)],
                         ["EBUSY", "ENOENT", "ENOENT", "ENOENT", "ENOENT", "ENOENT"])
        self.assertEqual((rows[-1]["terminal_count"], rows[-1]["node_frees"],
                          rows[-1]["tx_releases"], rows[-1]["slot_releases"]),
                         ("1", "1", "1", "1"))

    def test_cli_validates_all_arguments_before_producing_output(self):
        for args in ((), ("unknown",), ("method", "extra"), ("registry", "extra"),
                     ("trace",), ("trace", "call", "unknown")):
            result, rows = self.run_model(*args, expected=2)
            self.assertEqual(rows, [])
            self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
