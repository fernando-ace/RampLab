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
CLI = ROOT / "build-goal28/Release/airside_cli.exe"


@unittest.skipUnless(CLI.is_file(), "Build Release airside_cli before live-session integration tests.")
class LiveSessionTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
