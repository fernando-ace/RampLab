"""Persistent operational workflows and live-update service tests."""
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.ops_dashboard import scenario_studio
from tools.ops_dashboard.operational_store import OperationalStore, OperationalStoreError
from tools.ops_dashboard.operational_time import utc_now

FIXTURES = Path(__file__).parent / "fixtures" / "operational"
PROFILE = json.loads((FIXTURES / "kauo-demo-schedule-profile.json").read_text(encoding="utf-8"))
SCHEDULE = (FIXTURES / "kauo-tomorrow-schedule.csv").read_text(encoding="utf-8")
EVENTS = (FIXTURES / "kauo-synthetic-events.json").read_text(encoding="utf-8")


class OperationalStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ramplab-goal33-store-")
        self.addCleanup(self.temp.cleanup)
        self.store = OperationalStore(Path(self.temp.name), scenario_studio)
        self.dataset = self.store.import_schedule("tomorrow.csv", SCHEDULE, PROFILE)

    def test_dataset_identity_provenance_and_reusable_profiles_persist(self):
        self.assertEqual(self.dataset["scenario_ready"], True)
        self.assertEqual(self.store.get_profile("kauo-demo-schedule")["timezone"], "America/Chicago")
        self.assertEqual(self.store.list_datasets()[0]["source_sha256"], self.dataset["provenance"]["source_sha256"])
        replayed = self.store.import_schedule("tomorrow.csv", SCHEDULE, PROFILE)
        self.assertEqual(replayed["dataset_id"], self.dataset["dataset_id"])
        self.assertTrue((self.store._dataset_path(self.dataset["dataset_id"]) / "source.csv").is_file())
        self.assertEqual(self.store.get_dataset_export(self.dataset["dataset_id"])[0:2], b"PK")

    def test_schedule_scenario_runs_through_goal30_validation_and_carries_source_identity(self):
        result = self.store.create_plan_scenario(self.dataset["dataset_id"])
        self.assertTrue(result["validation"]["valid"], result["validation"])
        scenario = self.store.get_scenario(result["scenario_id"])
        self.assertEqual(scenario["dataset_id"], self.dataset["dataset_id"])
        self.assertEqual(scenario["operational_provenance"]["source_sha256"], self.dataset["provenance"]["source_sha256"])
        self.assertEqual(len(scenario["aircraft"]), 3)
        self.assertEqual(len(scenario["turnaround_requirements"]), 3)
        self.assertTrue(scenario["scenario_assumptions"])

    def test_historical_event_import_replay_pause_seek_and_plan_actual(self):
        imported = self.store.import_events(self.dataset["dataset_id"], "yesterday-demo.json", EVENTS)
        self.assertEqual(imported["event_count"], 8)
        self.assertEqual(imported["provenance"]["data_classification"], "SYNTHETIC")
        initial = self.store.replay_snapshot(imported["replay_id"])
        self.assertFalse(initial["state"]["flights"]["AX201"]["history"])
        stepped = self.store.control_replay(imported["replay_id"], "step")
        self.assertEqual(stepped["timestamp"], "2026-10-12T12:04:00Z")
        sought = self.store.control_replay(imported["replay_id"], "seek", "2026-10-12T12:25:00Z")
        self.assertEqual(sought["state"]["flights"]["AX201"]["status"], "SERVICE_COMPLETE")
        comparison = self.store.plan_actual(self.dataset["dataset_id"], imported["replay_id"])
        ax201 = next(row for row in comparison["rows"] if row["flight_id"] == "AX201")
        self.assertEqual(ax201["arrival_variance_minutes"], 4)
        self.assertEqual(ax201["departure_variance_minutes"], 8)
        self.assertEqual(self.store.export_replay(imported["replay_id"])[0:2], b"PK")

    def test_counterfactual_is_simulated_and_does_not_mutate_imported_baseline(self):
        replay = self.store.import_events(self.dataset["dataset_id"], "history.json", EVENTS)
        result = self.store.create_counterfactual(self.dataset["dataset_id"], "2026-10-12T12:20:00Z",
                                                  "KAUO-TWY-A-RAMP-CONNECTOR", 20, 30, replay["replay_id"])
        self.assertTrue(result["validation"]["valid"], result["validation"])
        scenario = result["scenario"]
        self.assertEqual(scenario["counterfactual"]["classification"], "SIMULATED")
        self.assertEqual(scenario["counterfactual"]["start_time"], "2026-10-12T12:40:00Z")
        self.assertEqual(len(self.store.get_dataset(self.dataset["dataset_id"])["flights"]), 3)
        self.assertEqual(scenario["flights"][0]["scheduled_time"], "2026-10-12T12:04:00Z")

    def test_live_updates_idempotency_out_of_order_and_recorded_replay(self):
        started = self.store.start_live_session(self.dataset["dataset_id"])
        session_id = started["session"]["session_id"]
        event_a = {"schema_version": "1.0", "session_id": session_id, "event_id": "live-arr", "event_type": "actual_arrival",
                   "timestamp": "2026-10-12T12:04:00Z", "entity_id": "AX201", "payload": {}, "source": "operator-console"}
        event_b = {"schema_version": "1.0", "session_id": session_id, "event_id": "live-dep", "event_type": "actual_departure",
                   "timestamp": "2026-10-12T12:53:00Z", "entity_id": "AX201", "payload": {}, "source": "operator-console"}
        late = {**event_a, "event_id": "live-service", "event_type": "service_completed", "timestamp": "2026-10-12T12:25:00Z"}
        received_at = utc_now()
        first = self.store.ingest_live_event(event_a, received_at=received_at)
        duplicate = self.store.ingest_live_event(event_a, received_at=received_at)
        second = self.store.ingest_live_event(event_b, received_at=received_at)
        late_result = self.store.ingest_live_event(late, received_at=received_at)
        self.assertFalse(first["duplicate"])
        self.assertTrue(duplicate["duplicate"])
        self.assertFalse(second["late"])
        self.assertTrue(late_result["late"])
        state = self.store.live_state(session_id)
        self.assertEqual(state["session"]["event_count"], 3)
        self.assertEqual(state["state"]["flights"]["AX201"]["status"], "DEPARTED")
        self.assertEqual(state["freshness"]["status"], "CURRENT")
        closed = self.store.close_live_session(session_id)
        self.assertEqual(closed["session"]["status"], "CLOSED")
        recording = self.store.recorded_replay(session_id)
        self.assertEqual(recording["event_count"], 3)
        with self.assertRaisesRegex(OperationalStoreError, "closed"):
            self.store.ingest_live_event(event_a, received_at="2026-10-12T12:57:00Z")

    def test_live_payload_validation_unknown_entities_and_freshness(self):
        started = self.store.start_live_session(self.dataset["dataset_id"])
        session_id = started["session"]["session_id"]
        base = {"schema_version": "1.0", "session_id": session_id, "event_id": "live-bad", "event_type": "gate_changed",
                "timestamp": "2026-10-12T13:00:00Z", "entity_id": "AX201", "payload": {}}
        with self.assertRaisesRegex(ValueError, "stand_id"):
            self.store.ingest_live_event(base, received_at="2026-10-12T13:00:00Z")
        valid = {**base, "event_id": "live-unknown", "event_type": "actual_arrival", "entity_id": "UNKNOWN", "payload": {}}
        with self.assertRaisesRegex(OperationalStoreError, "unknown entity"):
            self.store.ingest_live_event(valid, received_at="2026-10-12T13:00:00Z")

    def test_unknown_timezone_profile_is_not_saved(self):
        profile = {**PROFILE, "profile_id": "bad-zone", "timezone": "Nowhere/Imaginary"}
        with self.assertRaisesRegex(ValueError, "Unknown IANA timezone"):
            self.store.save_profile(profile)
        self.assertNotIn("bad-zone", {item["profile_id"] for item in self.store.list_profiles()})


if __name__ == "__main__":
    unittest.main()
