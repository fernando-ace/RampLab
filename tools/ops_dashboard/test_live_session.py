"""Native integration tests for Goal 28 live controls and deterministic replay."""

from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
import json
import os
import unittest
from unittest.mock import patch

from tools.ops_dashboard.live_session import LiveSession, LiveSessionError

ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / "build-goal34/Release/airside_cli.exe"
AGENT_CLI = CLI
GOAL41_CLI = ROOT / "build-goal41/Release/airside_cli.exe"
GOAL41_SCENARIO = ROOT / "docs/goal41-evidence/generated/disruption/scenario.json"


@unittest.skipUnless(CLI.is_file(), "Build Release airside_cli before live-session integration tests.")
class LiveSessionTests(unittest.TestCase):
    @unittest.skipUnless(AGENT_CLI.is_file(), "Build Goal 34 Release airside_cli before adaptive live-update tests.")
    def test_goal33_route_update_triggers_adaptive_agent_replan(self):
        with TemporaryDirectory(prefix="goal34-operational-update-") as directory:
            session = LiveSession(AGENT_CLI, Path(directory))
            try:
                state = session.start("ADAPTIVE")
                self.assertEqual(state["aircraft_routing_policy"], "ADAPTIVE")
                while state["simulated_time_seconds"] < 1504:
                    state = session.advance()
                self.assertEqual(state["simulated_time_seconds"], 1504)
                self.assertEqual(state["aircraft_routing_policy"], "ADAPTIVE")
                state = session.apply_operational_update({
                    "event_id": "goal33-closure-001", "event_type": "closure_started",
                    "entity_id": "KAUO-TWY-A-RAMP-CONNECTOR", "timestamp": "2026-10-12T14:25:12Z",
                    "source": "Goal 33 live operational feed", "payload": {"route_id": "KAUO-TWY-A-RAMP-CONNECTOR"}})
                self.assertEqual(state["applied_operational_update"]["source_event_id"], "goal33-closure-001")
                duplicate = session.apply_operational_update({
                    "event_id": "goal33-closure-001", "event_type": "closure_started",
                    "entity_id": "KAUO-TWY-A-RAMP-CONNECTOR", "timestamp": "2026-10-12T14:25:12Z",
                    "payload": {"route_id": "KAUO-TWY-A-RAMP-CONNECTOR"}})
                self.assertTrue(duplicate["duplicate"])
                result = session.finish()
                event_rows = [json.loads(line) for line in (session.output_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()]
                replans = [event for event in event_rows if event.get("type") == "AgentDecision"
                           and event.get("reason_code") == "OPERATOR_INTERVENTION"]
                self.assertTrue(replans, "Goal 33 route closure must be observed by the adaptive aircraft agent.")
                metrics = json.loads((session.output_dir / "simulator-metrics.json").read_text(encoding="utf-8"))
                self.assertEqual(metrics["surface"]["safe_failures"], 0)
                self.assertEqual(metrics["surface"]["aircraft_aircraft_collisions"], 0)
                self.assertEqual(metrics["surface"]["aircraft_ground_collisions"], 0)
            finally:
                session.close()

    def test_live_state_replace_retries_a_transient_reader_lock(self):
        with TemporaryDirectory(prefix="goal28-state-file-") as directory:
            session = LiveSession(CLI, Path(directory))
            real_replace = os.replace
            attempts = 0

            def transient_lock(source, target):
                nonlocal attempts
                attempts += 1
                if attempts < 3:
                    raise PermissionError("simulated Unreal read lock")
                return real_replace(source, target)

            try:
                session.snapshot["session_state"] = "paused"
                with patch("tools.ops_dashboard.live_session.os.replace", side_effect=transient_lock):
                    session._write_live_state()
                self.assertEqual(attempts, 3)
                self.assertEqual(json.loads((Path(directory) / "live-state.json").read_text())["session_state"], "paused")
            finally:
                session.close()

    def test_start_pause_resume_reset_and_deterministic_speed_controls(self):
        with TemporaryDirectory(prefix="goal28-lifecycle-") as directory:
            session = LiveSession(CLI, Path(directory))
            try:
                first = session.start()
                self.assertEqual((first["scenario"], first["seed"], first["session_state"]),
                                 ("airport_kauo", 42, "paused"))
                state_file = Path(first["live_state_file"])
                self.assertEqual(json.loads(state_file.read_text(encoding="utf-8"))["session_state"], "paused")
                for speed in ("1x", "2x", "5x", "max"):
                    self.assertEqual(session.set_speed(speed)["speed"], speed)
                self.assertEqual(session.pause()["session_state"], "paused")
                self.assertEqual(session.resume()["session_state"], "running")
                self.assertEqual(json.loads(state_file.read_text(encoding="utf-8"))["session_state"], "running")
                self.assertEqual(session.pause()["session_state"], "paused")
                reset = session.reset()
                self.assertEqual(reset["simulated_time_seconds"], 0)
                self.assertEqual(reset["session_state"], "paused")
                with self.assertRaises(LiveSessionError):
                    session.set_speed("3x")
            finally:
                session.close()

    def test_closure_outage_event_provenance_ordering_and_replay(self):
        with TemporaryDirectory(prefix="goal28-replay-") as directory:
            session = LiveSession(CLI, Path(directory))
            try:
                state = session.start()
                actions = ((15, "surface_closure", 4, "twy_a_mid_fbo_primary"),
                           (2500, "equipment_outage", 3, "KAUO-SIM-LOAD-01"))
                for at, kind, target, _ in actions:
                    while state["simulated_time_seconds"] < at:
                        state = session.advance()
                    self.assertEqual(state["simulated_time_seconds"], at)
                    state = session.intervene(kind, target)
                live_state = json.loads((session.root / "live-state.json").read_text(encoding="utf-8"))
                self.assertEqual(live_state["session_state"], "paused")
                self.assertEqual(live_state["simulated_time_seconds"], 2500)
                first = session.finish()
                first_dir = session.output_dir
                self.assertEqual([row["simulated_time_seconds"] for row in session.interventions], [15, 2500])
                self.assertEqual([row["target_name"] for row in session.interventions],
                                 ["twy_a_mid_fbo_primary", "KAUO-SIM-LOAD-01"])
                self.assertEqual([row["sequence"] for row in session.interventions],
                                 sorted(row["sequence"] for row in session.interventions))
                event_types = [event["type"] for event in first["events"]]
                intervention_indexes = [i for i, kind in enumerate(event_types) if kind == "OperatorIntervention"]
                self.assertEqual(len(intervention_indexes), 2)
                self.assertGreater(event_types.index("RoadClosed"), intervention_indexes[0])
                log = list(session.interventions)
                replayed = session.replay(log)
                replay_dir = session.output_dir
                self.assertEqual(replayed["session_state"], "completed")
                for artifact in ("simulator-metrics.json", "events.jsonl", "interventions.json"):
                    self.assertEqual((first_dir / artifact).read_bytes(), (replay_dir / artifact).read_bytes(), artifact)
                saved = (replay_dir / "events.jsonl").read_text(encoding="utf-8")
                self.assertIn('"type":"OperatorIntervention"', saved)
                self.assertIn('"type":"RoadClosed"', saved)
            finally:
                session.close()

    def test_invalid_intervention_targets_are_rejected_by_simulator(self):
        with TemporaryDirectory(prefix="goal28-invalid-") as directory:
            session = LiveSession(CLI, Path(directory))
            try:
                session.start()
                with self.assertRaises(LiveSessionError):
                    session.intervene("surface_closure", 999999)
                with self.assertRaises(LiveSessionError):
                    session.intervene("equipment_outage", 999999)
            finally:
                session.close()

class ConfiguredScenarioTests(unittest.TestCase):
    @unittest.skipUnless(GOAL41_CLI.is_file() and GOAL41_SCENARIO.is_file(),
                         "Build Goal 41 Release airside_cli and generate its evidence scenario.")
    def test_goal41_live_run_finishes_and_exports_the_configured_operation(self):
        with TemporaryDirectory(prefix="goal41-live-completion-") as directory:
            session = LiveSession(GOAL41_CLI, Path(directory) / "session", GOAL41_SCENARIO)
            try:
                state = session.start()
                self.assertEqual(state["session_state"], "paused")
                self.assertEqual(state["aircraft_routing_policy"], "ADAPTIVE")
                self.assertEqual(len(state["aircraft"]), 3)
                self.assertEqual(len(state["vehicles"]), 9)
                self.assertEqual(len(state["tasks"]), 33)
                while not state.get("finished"):
                    state = session.advance()
                self.assertFalse(session.state()["artifacts_ready"])
                state = session.finish()
                self.assertEqual(state["session_state"], "completed")
                self.assertTrue(state["artifacts_ready"])
                run = session.artifacts()["run"]
                files = {item["name"]: item["content"] for item in run["files"]}
                self.assertIn("experiment.json", files)
                self.assertIn("events.jsonl", files)
                experiment = json.loads(files["experiment.json"])
                self.assertIn("TurnaroundScheduleAdjusted", {
                    item.get("action") for item in experiment["operator_decision_summaries"]})
                self.assertTrue(any(json.loads(line).get("type") == "TurnaroundDisruptionDetected"
                                     for line in files["events.jsonl"].splitlines() if line.strip()))
            finally:
                session.close()

    def test_goal41_service_delay_is_exposed_as_a_planned_schedule_disruption(self):
        with TemporaryDirectory(prefix="goal41-configured-live-") as directory:
            root = Path(directory)
            scenario_dir = root / "disruption"
            scenario_dir.mkdir()
            scenario = scenario_dir / "scenario.json"
            scenario.write_text(json.dumps({
                "default_seed": 42,
                "surface_operations": {"aircraft_routing_policy": "ADAPTIVE"},
                "aircraft": [{"id": "KAUO-G31-GA-03", "service_tasks": [
                    {"id": "G31-03-PUSH", "type": "Pushback", "duration_seconds": 180}]}],
                "turnaround_disruptions": [{"time_seconds": 241, "aircraft": "KAUO-G31-GA-03",
                                             "task": "G31-03-PUSH", "duration_seconds": 480}],
            }), encoding="utf-8")
            session = LiveSession(root=root / "session", scenario_file=scenario)
            try:
                state = session.state()
                self.assertEqual(state["planned_disruption"]["kind"], "service_delay")
                self.assertEqual(state["planned_disruption"]["service_type"], "Pushback")
                self.assertEqual(state["planned_disruption"]["base_duration_seconds"], 180)
                self.assertEqual(state["planned_disruption"]["duration_seconds"], 480)
                self.assertEqual(state["planned_disruption"]["added_duration_seconds"], 300)
            finally:
                session.close()

    def test_goal41_configured_scenario_exposes_policy_seed_and_closure_before_start(self):
        with TemporaryDirectory(prefix="goal41-configured-live-") as directory:
            root = Path(directory)
            scenario_dir = root / "disruption"
            scenario_dir.mkdir()
            scenario = scenario_dir / "scenario.json"
            scenario.write_text(json.dumps({
                "default_seed": 42,
                "surface_operations": {"aircraft_routing_policy": "COUNTERFACTUAL"},
                "road_events": [{"time_seconds": 15, "edge": "twy_a_mid_fbo_primary", "enabled": False}],
            }), encoding="utf-8")
            session = LiveSession(root=root / "session", scenario_file=scenario)
            try:
                state = session.state()
                self.assertEqual(state["seed"], 42)
                self.assertEqual(state["aircraft_routing_policy"], "COUNTERFACTUAL")
                self.assertEqual(state["planned_disruption"]["edge_id"], "twy_a_mid_fbo_primary")
            finally:
                session.close()


if __name__ == "__main__":
    unittest.main()
