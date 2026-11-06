import itertools
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReferenceModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mini-binder-ref-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "references"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pedantic", str(ROOT / "examples/models/references.c"),
                        "-o", str(cls.binary)], check=True, capture_output=True,
                       text=True, timeout=30)

    def run_model(self, *arguments, expected=0):
        result = subprocess.run([str(self.binary), *arguments], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        rows = [dict(field.split("=", 1) for field in line.split())
                for line in result.stdout.splitlines()]
        return result, rows

    def trace(self, *operations):
        _, rows = self.run_model("trace", *operations)
        snapshots = []
        for row in rows:
            if row["event"] == "step":
                snapshots.append({"step": row, "node": [], "ref": [],
                                  "slot": [], "hold": [], "tx": []})
            else:
                self.assertTrue(snapshots, "observation before step")
                self.assertIn(row["event"], snapshots[-1])
                snapshots[-1][row["event"]].append(row)
        self.assertEqual([s["step"]["operation"] for s in snapshots],
                         ["init", *operations])
        for snapshot in snapshots:
            self.assert_graph(snapshot)
        return snapshots[1:]

    def assert_graph(self, snapshot):
        """Rebuild holds from Ref/slot/transaction observations, not totals."""
        step = snapshot["step"]
        nodes = {int(n["id"]): n for n in snapshot["node"]}
        refs = {(r["context"], int(r["handle"])): r for r in snapshot["ref"]}
        slots = {(s["context"], int(s["token"])): s for s in snapshot["slot"]}
        self.assertEqual(len(refs), len(snapshot["ref"]))
        self.assertEqual(len(slots), len(snapshot["slot"]))
        pairs = [(r["context"], r["node"]) for r in refs.values()]
        self.assertEqual(len(set(pairs)), len(pairs), "duplicate Ref for Node")
        expected_buffer = {key: 0 for key in refs}
        local = {node: 0 for node in nodes}
        slot_targets = {node: 0 for node in nodes}
        ref_entries = {node: 0 for node in nodes}
        tx_targets = {node: 0 for node in nodes}
        hold_keys = [(h["context"], int(h["token"]), int(h["index"]))
                     for h in snapshot["hold"]]
        self.assertEqual(len(hold_keys), len(set(hold_keys)))
        for context, token, _ in hold_keys:
            self.assertIn((context, token), slots, "orphan hold without a slot")
        for ref in refs.values():
            self.assertGreater(int(ref["handle"]), 0)
            self.assertGreaterEqual(int(ref["user"]), 0)
            self.assertGreaterEqual(int(ref["buffer"]), 0)
            self.assertGreater(int(ref["user"]) + int(ref["buffer"]), 0)
            self.assertEqual(nodes[int(ref["node"])]["allocated"], "1")
            ref_entries[int(ref["node"])] += 1
        for slot in slots.values():
            if int(slot["target"]):
                slot_targets[int(slot["target"])] += 1
            holds = [h for h in snapshot["hold"]
                     if (h["context"], h["token"]) == (slot["context"], slot["token"])]
            self.assertEqual(len(holds), int(slot["objects"]))
            self.assertEqual([int(h["index"]) for h in holds], list(range(len(holds))))
            for hold in holds:
                node = int(hold["node"])
                if hold["type"] == "HANDLE":
                    key = (hold["context"], int(hold["handle"]))
                    self.assertIn(key, refs)
                    self.assertEqual(refs[key]["node"], hold["node"])
                    self.assertEqual(hold["cookie"], "0")
                    expected_buffer[key] += 1
                else:
                    self.assertEqual(hold["type"], "LOCAL")
                    self.assertEqual(hold["handle"], "0")
                    self.assertEqual(nodes[node]["owner"], slot["context"])
                    self.assertEqual(hold["cookie"], nodes[node]["cookie"])
                    local[node] += 1
        for key, ref in refs.items():
            self.assertEqual(int(ref["buffer"]), expected_buffer[key])
        for tx in snapshot["tx"]:
            tx_targets[int(tx["node"])] += 1
        live = 0
        for node, row in nodes.items():
            holds = (int(row["published"]) + ref_entries[node] + local[node]
                     + slot_targets[node] + tx_targets[node])
            self.assertEqual(int(row["refs"]), ref_entries[node])
            self.assertEqual(int(row["local"]), local[node])
            self.assertEqual(int(row["target_slots"]), slot_targets[node])
            self.assertEqual(int(row["transactions"]), tx_targets[node])
            self.assertEqual(int(row["allocated"]), int(holds != 0))
            live += int(holds != 0)
        self.assertEqual(int(step["nodes"]), live)
        self.assertEqual(int(step["allocs"]) - int(step["frees"]), live)
        self.assertEqual(int(step["refs"]), len(refs))
        self.assertEqual(int(step["slots"]), len(slots))
        self.assertEqual(int(step["users"]), sum(int(r["user"]) for r in refs.values()))
        self.assertEqual(int(step["buffers"]), len([h for h in snapshot["hold"]
                                                   if h["type"] == "HANDLE"]))
        self.assertEqual(int(step["locals"]), sum(local.values()))
        self.assertEqual(int(step["pending"]), len([s for s in slots.values()
                                                   if s["kind"] == "GRANT" and s["delivered"] == "0"]))

    @staticmethod
    def refs(snapshot):
        return {(r["context"], int(r["handle"])):
                (int(r["node"]), int(r["user"]), int(r["buffer"]))
                for r in snapshot["ref"]}

    def test_raw_integer_looks_up_a_different_table_and_object(self):
        _, rows = self.run_model("raw")
        results = [r for r in rows if r["event"] == "result"]
        self.assertEqual([(r["context"], r["handle"], r["status"], r["node"], r["value"])
                          for r in results],
                         [("C", "1", "OK", "102", "101"),
                          ("C", "2", "EBADF", "0", "none")])

    def test_transfer_uses_real_mapping_and_independent_counter_state(self):
        _, rows = self.run_model("transfer")
        results = [r for r in rows if r["event"] == "result"]
        self.assertEqual([(r["context"], r["handle"], r["node"], r["value"])
                          for r in results], [("C", "2", "101", "1"),
                                               ("B", "1", "101", "3"),
                                               ("C", "1", "102", "101")])
        self.assertEqual(rows[-1], {"event": "summary", "nodes": "0", "refs": "0",
                                    "slots": "0", "pending": "0"})

    def test_grant_is_bound_and_pending_cannot_be_freed(self):
        rows = self.trace("send_x", "wrong_import", "free_x", "import_x", "import_x")
        self.assertEqual([r["step"]["status"] for r in rows],
                         ["OK", "ENOENT", "EBUSY", "OK", "ENOENT"])
        self.assertEqual(self.refs(rows[0])[("C", 2)], (101, 0, 1))

    def test_pending_is_already_authorized_and_acquire_does_not_consume_buffer(self):
        rows = self.trace("send_x", "acquire_x", "import_x", "free_x", "use_x", "release_x")
        self.assertEqual(self.refs(rows[1])[("C", 2)], (101, 1, 1))
        self.assertEqual(self.refs(rows[3])[("C", 2)], (101, 1, 0))
        self.assertEqual((rows[4]["step"]["node"], rows[4]["step"]["value"]), ("101", "1"))
        self.assertNotIn(("C", 2), self.refs(rows[-1]))

    def test_buffer_only_reference_cannot_be_user_released(self):
        rows = self.trace("send_x", "release_x", "import_x", "free_x", "release_x")
        self.assertEqual([r["step"]["status"] for r in rows],
                         ["OK", "EINVAL", "OK", "OK", "EBADF"])

    def test_import_output_fault_preserves_exact_grant_for_retry(self):
        rows = self.trace("send_x", "import_fault", "import_fault", "import_x")
        self.assertEqual(self.refs(rows[0]), self.refs(rows[1]))
        self.assertEqual(rows[0]["slot"], rows[1]["slot"])
        self.assertEqual(rows[0]["hold"], rows[2]["hold"])
        self.assertEqual(rows[-1]["step"]["pending"], "0")

    def test_duplicate_objects_share_handle_but_each_owns_a_buffer_unit(self):
        rows = self.trace("send_twice", "acquire_x", "import_x", "free_x")
        self.assertEqual(self.refs(rows[0])[("C", 2)], (101, 0, 2))
        self.assertEqual(len(rows[0]["hold"]), 2)
        self.assertEqual(self.refs(rows[-1])[("C", 2)], (101, 1, 0))

    def test_full_sixteen_object_ledger_is_released(self):
        rows = self.trace("send_max", "import_x", "free_x")
        self.assertEqual(self.refs(rows[0])[("C", 2)], (101, 0, 16))
        self.assertEqual(rows[-1]["step"]["buffers"], "0")

    def test_pending_grants_share_sixteen_slots_and_close_releases_every_slot(self):
        rows = self.trace(*(["send_x"] * 17), "close_c")
        self.assertTrue(all(r["step"]["status"] == "OK" for r in rows[:16]))
        self.assertEqual((rows[16]["step"]["status"], rows[16]["step"]["slots"],
                          rows[16]["step"]["pending"], rows[16]["step"]["converted"]),
                         ("EAGAIN", "16", "16", "0"))
        self.assertEqual((rows[-1]["step"]["slots"], rows[-1]["step"]["buffers"]), ("0", "0"))

    def test_partial_failure_rolls_back_new_ref_and_reused_user_ref(self):
        for operation in ("send_bad", "send_bad_twice", "send_fault"):
            with self.subTest(operation=operation):
                fresh = self.trace(operation)
                self.assertEqual(fresh[0]["step"]["status"],
                                 "EFAULT" if operation == "send_fault" else "EBADF")
                prefix = "2" if operation == "send_bad_twice" else "1"
                self.assertEqual((fresh[0]["step"]["converted"],
                                  fresh[0]["step"]["rolled_back"]), (prefix, prefix))
                self.assertEqual(self.refs(fresh[0]),
                                 {("B", 1): (101, 1, 0), ("B", 2): (102, 1, 0),
                                  ("C", 1): (102, 1, 0)})
                reused = self.trace("send_x", "acquire_x", "import_x", "free_x", operation)
                self.assertEqual(self.refs(reused[-1]), self.refs(reused[-2]))
                self.assertEqual(reused[-1]["slot"], reused[-2]["slot"])
                self.assertEqual((reused[-1]["step"]["converted"],
                                  reused[-1]["step"]["rolled_back"]), (prefix, prefix))

    def test_partial_failure_preserves_existing_buffer_units(self):
        rows = self.trace("send_twice", "send_bad_twice")
        self.assertEqual(self.refs(rows[0]), self.refs(rows[1]))
        self.assertEqual(rows[0]["slot"], rows[1]["slot"])

    def test_forged_local_wrong_cookie_and_unknown_type_do_not_grant_refs(self):
        rows = self.trace("forge_local", "wrong_cookie", "send_unknown", "forged_handle")
        self.assertEqual([r["step"]["status"] for r in rows],
                         ["EPERM", "EINVAL", "EOPNOTSUPP", "EBADF"])
        self.assertTrue(all(r["step"]["refs"] == "3" for r in rows))
        self.assertEqual((rows[2]["step"]["converted"], rows[2]["step"]["rolled_back"]),
                         ("0", "0"))

    def test_pending_handle_can_be_called_before_import(self):
        rows = self.trace("send_x", "use_x", "import_x", "free_x", "use_x")
        self.assertEqual((rows[1]["step"]["status"], rows[1]["step"]["value"],
                          rows[1]["step"]["pending"]), ("OK", "1", "1"))
        self.assertEqual(rows[-1]["step"]["status"], "EBADF")

    def test_owner_receives_local_cookie_without_a_self_ref(self):
        rows = self.trace("return_owner", "import_owner", "free_owner")
        self.assertEqual(rows[0]["step"]["refs"], "3")
        self.assertEqual([(h["type"], h["node"], h["handle"], h["cookie"])
                          for h in rows[0]["hold"]], [("LOCAL", "101", "0", "11")])
        self.assertEqual(rows[0]["step"]["locals"], "1")
        self.assertEqual(rows[-1]["step"]["locals"], "0")

    def test_sender_exit_does_not_retract_successful_pending_grant(self):
        rows = self.trace("send_x", "close_b", "import_x", "use_x")
        self.assertEqual(rows[1]["step"]["pending"], "1")
        self.assertEqual((rows[-1]["step"]["status"], rows[-1]["step"]["node"]),
                         ("OK", "101"))

    def test_dead_owner_ref_is_not_callable_but_import_release_free_work(self):
        rows = self.trace("send_x", "acquire_x", "close_a", "use_x", "send_x",
                          "acquire_x", "import_x", "release_x", "free_x")
        self.assertEqual([r["step"]["status"] for r in rows],
                         ["OK", "OK", "OK", "EPIPE", "EPIPE", "EPIPE", "OK", "OK", "OK"])
        self.assertNotIn(("C", 2), self.refs(rows[-1]))

    def test_minimum_free_handle_can_name_another_node(self):
        rows = self.trace("send_x", "import_x", "free_x", "send_z", "import_z", "stale_x")
        self.assertEqual(self.refs(rows[3])[("C", 2)], (103, 0, 1))
        self.assertEqual((rows[-1]["step"]["node"], rows[-1]["step"]["value"]),
                         ("103", "1001"))

    def test_inflight_target_outlives_ref_and_borrowed_slot(self):
        rows = self.trace("send_x", "import_x", "call", "free_x", "send_z", "deliver",
                          "free_request", "reply", "reap")
        self.assertEqual(rows[2]["step"]["node"], "101")
        self.assertEqual(self.refs(rows[4])[("C", 2)][0], 103)
        self.assertEqual(rows[7]["step"]["node"], "101")
        self.assertEqual(rows[7]["step"]["value"], "1")
        target = next(n for n in rows[6]["node"] if n["id"] == "101")
        self.assertEqual((target["transactions"], target["target_slots"], target["local"]),
                         ("1", "0", "0"))

    def test_queued_and_delivered_cancel_have_separate_object_responsibilities(self):
        for delivered in (False, True):
            with self.subTest(delivered=delivered):
                prefix = ["send_x", "import_x", "call"]
                if delivered:
                    prefix.append("deliver")
                rows = self.trace(*prefix, "cancel", "reap")
                self.assertEqual(rows[-1]["step"]["locals"], "1" if delivered else "0")
                self.assertEqual(rows[-1]["step"]["state"], "FAILED")
                if delivered:
                    end = self.trace(*prefix, "cancel", "reap", "free_request")
                    self.assertEqual(end[-1]["step"]["locals"], "0")

    def test_receive_fault_and_reply_fault_allow_retry_without_extra_holds(self):
        rows = self.trace("send_x", "import_x", "call", "receive_fault", "deliver",
                          "reply_fault", "reply_object", "reap", "return_reply", "free_reply")
        self.assertEqual(rows[3]["step"]["state"], "QUEUED")
        self.assertEqual(rows[5]["step"]["state"], "DELIVERED")
        self.assertEqual(self.refs(rows[5])[("C", 1)], (102, 1, 0))
        self.assertEqual(self.refs(rows[6])[("C", 1)], (102, 1, 1))
        self.assertEqual(self.refs(rows[-1])[("C", 1)], (102, 1, 0))

    def test_unexposed_reply_blocks_new_call_after_reap_until_output_is_handled(self):
        rows = self.trace("send_x", "import_x", "call", "deliver", "reply_object", "reap",
                          "call", "return_reply", "call", "deliver", "reply_object", "reap",
                          "call", "call_output_fault", "call")
        self.assertEqual(rows[6]["step"]["status"], "EBUSY")
        self.assertEqual(rows[7]["step"]["status"], "OK")
        self.assertEqual(rows[8]["step"]["status"], "OK")
        self.assertEqual(rows[12]["step"]["status"], "EBUSY")
        self.assertEqual(rows[13]["step"]["status"], "EFAULT")
        self.assertEqual(rows[14]["step"]["status"], "OK")

    def test_call_output_fault_rolls_back_only_reply_embedded_units(self):
        rows = self.trace("send_x", "import_x", "call", "deliver", "reply_object", "reap",
                          "call_output_fault", "free_request")
        self.assertEqual(rows[6]["step"]["status"], "EFAULT")
        self.assertEqual(self.refs(rows[6])[("C", 1)], (102, 1, 0))
        self.assertEqual(rows[6]["step"]["locals"], "1")
        self.assertEqual(rows[-1]["step"]["locals"], "0")

    def test_close_orders_release_all_nodes_refs_and_slot_units(self):
        for prefix in (("send_twice",), ("send_x", "import_x", "call"),
                       ("send_x", "import_x", "call", "deliver", "reply_object")):
            for order in itertools.permutations(("close_a", "close_b", "close_c")):
                with self.subTest(prefix=prefix, order=order):
                    row = self.trace(*prefix, *order)[-1]["step"]
                    self.assertEqual(tuple(row[k] for k in ("nodes", "refs", "slots", "pending")),
                                     ("0", "0", "0", "0"))

    def test_unmap_does_not_allow_remap_or_import_but_final_close_cleans_pending(self):
        rows = self.trace("send_x", "unmap_c", "import_x", "remap_c", "free_x", "close_c")
        self.assertEqual([r["step"]["status"] for r in rows],
                         ["OK", "OK", "ENXIO", "EBUSY", "EBUSY", "OK"])
        self.assertEqual(rows[-1]["step"]["pending"], "0")

    def test_repeated_terminal_and_cleanup_operations_cannot_double_release(self):
        rows = self.trace("send_x", "import_x", "call", "cancel", "cancel", "reply", "reap",
                          "reap", "free_request", "free_x", "free_x")
        self.assertEqual([rows[i]["step"]["status"] for i in (4, 5, 7, 8, 10)],
                         ["ENOENT"] * 5)

    def test_cli_rejects_entire_invalid_sequence_before_any_output(self):
        for arguments in ((), ("unknown",), ("raw", "extra"), ("transfer", "extra"),
                          ("trace",), ("trace", "send_x", "unknown")):
            result, rows = self.run_model(*arguments, expected=2)
            self.assertEqual(rows, [])
            self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
