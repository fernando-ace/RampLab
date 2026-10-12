"""Focused tests for generic Goal 32 experiment generation and scheduling."""
from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import tempfile
import threading
import time
import unittest

from tools.ops_dashboard import experiments


class FakeStudio:
    ROOT = Path(__file__).resolve().parents[2]
    HOME = Path(tempfile.gettempdir()) / "ramplab-goal32-tests"

    @staticmethod
    def _mapping(key): return Path(key)

    @staticmethod
    def load_mapping(_path): return ({"route_ids": {"ROUTE-A": "EDGE-A"}}, None, {}, None)

    @staticmethod
    def scenario_package(scenario): return deepcopy(scenario)

    @staticmethod
    def validate_scenario(_scenario): return {"valid": True, "findings": []}

    @staticmethod
    def _resolve_cli(): return Path("airside_cli")


def base_scenario():
    return {"schema_version": 1, "airport_key": "kauo", "dataset_id": "test-dataset", "seed": 42,
            "flights": [{"flight_id": "ARR", "operation": "arrival", "scheduled_time": "2026-10-09T14:00:00Z"},
                        {"flight_id": "DEP", "operation": "departure", "scheduled_time": "2026-10-09T15:00:00Z"}],
            "disruptions": [], "agent_policy": {"aircraft_routing": "SCRIPTED"}}


def definition(**updates):
    value = {"name": "test", "base_dataset": "test-dataset", "base_scenario": base_scenario(),
             "parameters": [{"id": "disruption.route_closure.start_time", "values": ["2026-10-09T14:00:00Z", "2026-10-09T14:15:00Z"]},
                            {"id": "disruption.route_closure.duration_minutes", "values": [15, 30]}],
             "seeds": [42, 43], "execution": {"workers": 3}, "include_baseline": True,
             "constraints": [{"metric": "surface_aircraft_aircraft_collisions", "op": "eq", "value": 0}],
             "ranking": {"objectives": [{"metric": "avg_departure_delay_minutes", "direction": "minimize"}]}}
    value.update(updates)
    return value


class ExperimentDefinitionTests(unittest.TestCase):
    def test_registry_only_exposes_supported_flight_and_mapped_closure(self):
        entries = experiments.registry(base_scenario(), FakeStudio)
        self.assertEqual({item["adapter"] for item in entries}, {"path", "closure_start", "closure_duration"})
        self.assertEqual(entries[-2]["route_id"], "ROUTE-A")
        policy = next(item for item in entries if item["id"] == "agent_policy.aircraft_routing")
        self.assertEqual(policy["allowed_values"], ["SCRIPTED", "ADAPTIVE"])
        varied = experiments._apply_parameters(base_scenario(), {policy["id"]: "ADAPTIVE"}, {policy["id"]: policy})
        self.assertEqual(varied["agent_policy"]["aircraft_routing"], "ADAPTIVE")

    def test_cartesian_generation_seed_expansion_and_stable_ids(self):
        left = experiments._configurations(definition(), FakeStudio)[0]
        right = experiments._configurations(definition(), FakeStudio)[0]
        self.assertEqual(len(left), 5)  # 2 x 2 sweep plus unchanged baseline
        self.assertEqual([row["configuration_id"] for row in left], [row["configuration_id"] for row in right])
        self.assertEqual(len(experiments._configurations(definition(), FakeStudio)[1]), 2)
        first = next(row for row in left if row["parameters"].get("disruption.route_closure.duration_minutes") == 15)
        closure = first["scenario"]["disruptions"][0]
        self.assertEqual(closure["start_time"], "2026-10-09T14:00:00Z")
        self.assertEqual(closure["end_time"], "2026-10-09T14:15:00Z")

    def test_inclusive_numeric_ranges_and_bad_ranges(self):
        self.assertEqual(experiments._axis_values({"range": {"start": 15, "stop": 45, "step": 15}}), [15, 30, 45])
        with self.assertRaisesRegex(ValueError, "step"):
            experiments._axis_values({"range": {"start": 45, "stop": 15, "step": 3}})
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            experiments._axis_values({"values": [1, 1]})

    def test_registered_scenario_paths_support_turnaround_and_fleet_fields(self):
        scenario = base_scenario()
        scenario["turnaround_requirements"] = [{"requirement_id": "FUEL-A", "duration_minutes": 20}]
        scenario["equipment"] = [{"equipment_id": "TRUCK-A", "status": "available"}]
        entries = {item["id"]: item for item in experiments.registry(scenario, FakeStudio)}
        scenario["turnaround_requirements"][0]["duration_minutes"] = 35
        scenario["equipment"][0]["status"] = "unavailable"
        definitions = {key: item for key, item in entries.items()}
        varied = experiments._apply_parameters(scenario, {"turnaround.FUEL-A.duration_minutes": 40,
                                                           "fleet.TRUCK-A.status": "unavailable"}, definitions)
        self.assertEqual(varied["turnaround_requirements"][0]["duration_minutes"], 40)
        self.assertEqual(varied["equipment"][0]["status"], "unavailable")
        with self.assertRaisesRegex(ValueError, "one of"):
            experiments._validate_value(entries["fleet.TRUCK-A.status"], "removed")

    def test_run_count_guard_and_explicit_override(self):
        wide = definition(parameters=[{"id": "flight.DEP.scheduled_time", "values": [f"2026-10-09T15:{minute:02d}:00Z" for minute in range(0, 55, 5)]}],
                          seeds=list(range(10)), include_baseline=False)
        with self.assertRaisesRegex(ValueError, "110 runs"):
            experiments._configurations(wide, FakeStudio)
        wide["allow_large"] = True
        self.assertEqual(len(experiments._configurations(wide, FakeStudio)[0]), 11)

    def test_unknown_and_duplicate_parameters_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            experiments._configurations(definition(parameters=[{"id": "turnaround.gse_quantity", "values": [2]}]), FakeStudio)
        duplicate = definition(parameters=[{"id": "flight.DEP.scheduled_time", "values": ["2026-10-09T15:00:00Z"]},
                                           {"id": "flight.DEP.scheduled_time", "values": ["2026-10-09T15:15:00Z"]}])
        with self.assertRaisesRegex(ValueError, "Duplicate parameter"):
            experiments._configurations(duplicate, FakeStudio)

    def test_constraints_distinguish_missing_from_infeasible_data(self):
        self.assertEqual(experiments._feasibility([{"status": "COMPLETED", "metrics": {}}], {},
                                                   [{"metric": "surface_safe_failures", "op": "eq", "value": 0}]), "INSUFFICIENT DATA")
        self.assertEqual(experiments._feasibility([{"status": "COMPLETED", "metrics": {"surface_safe_failures": 1}}], {},
                                                   [{"metric": "surface_safe_failures", "op": "eq", "value": 0}]), "INFEASIBLE")

    def test_ranking_is_stable_and_frontier_preserves_tradeoffs(self):
        configs = [
            {"configuration_id": "b", "status": "FEASIBLE", "metrics": {"delay": {"mean": 2}, "fleet": {"mean": 3}}},
            {"configuration_id": "a", "status": "FEASIBLE", "metrics": {"delay": {"mean": 2}, "fleet": {"mean": 3}}},
            {"configuration_id": "c", "status": "FEASIBLE", "metrics": {"delay": {"mean": 1}, "fleet": {"mean": 5}}},
            {"configuration_id": "d", "status": "FEASIBLE", "metrics": {"delay": {"mean": 3}, "fleet": {"mean": 4}}},
            {"configuration_id": "unsafe", "status": "INFEASIBLE", "metrics": {"delay": {"mean": 0}, "fleet": {"mean": 1}}},
        ]
        result = experiments._rank(configs, [{"metric": "delay", "direction": "minimize"}, {"metric": "fleet", "direction": "minimize"}])
        ranks = {item["configuration_id"]: item.get("rank") for item in result}
        self.assertEqual(ranks["c"], 1); self.assertEqual(ranks["a"], 2); self.assertEqual(ranks["b"], 3)
        self.assertIsNone(ranks["unsafe"])
        self.assertEqual({item["configuration_id"] for item in result if item["pareto"]}, {"a", "b", "c"})

    def test_report_includes_baseline_delta_observed_response_and_limits(self):
        metric = lambda value: {"mean": value, "unit": "min"}
        data = {"name": "Report test", "id": "a" * 20, "base_dataset": "synthetic-test",
                "counts": {"configurations": 2, "seeds": 2, "total": 4, "completed": 4}, "seeds": [1, 2],
                "execution": {"workers": 2}, "parameters": [{"id": "closure.minutes", "name": "Closure duration", "values": [15, 30]}],
                "constraints": [], "ranking": {"objectives": [{"metric": "delay", "direction": "minimize"}]},
                "selected_candidate": "config-a", "provenance": {"software_revision": "abc"},
                "ranking_result": [
                    {"configuration_id": "baseline_a", "parameters": {"baseline": True}, "status": "FEASIBLE", "metrics": {"delay": metric(5)}},
                    {"configuration_id": "config-a", "parameters": {"closure.minutes": 15}, "status": "FEASIBLE", "rank": 1,
                     "metrics": {"delay": metric(3)}, "pareto": True},
                    {"configuration_id": "config-b", "parameters": {"closure.minutes": 30}, "status": "FEASIBLE",
                     "metrics": {"delay": metric(4)}, "pareto": False},
                ]}
        report = experiments.report_markdown(data)
        self.assertIn("## Baseline comparison", report)
        self.assertIn("-2.000 min", report)
        self.assertIn("## Observed parameter response", report)
        self.assertIn("not causal estimates", report)


class ExperimentSchedulerTests(unittest.TestCase):
    def test_start_recovers_interrupted_run_and_preserves_partial_output(self):
        with tempfile.TemporaryDirectory(prefix="ramplab-goal32-resume-") as temp:
            manager = experiments.ExperimentManager(Path(temp), FakeStudio)
            manager._ensure_cli = lambda: Path("fake-airside-cli")
            exp = manager.create(definition())
            run = exp["runs"][0]
            partial = Path(temp) / "partial-output"
            partial.mkdir(); (partial / "marker.txt").write_text("partial", encoding="utf-8")
            run.update(status="RUNNING", output_path=str(partial), validation_status="RUNNING")
            exp["status"] = "RUNNING"
            manager._write(exp)

            manager._execute_run = lambda *_: {"status": "COMPLETED", "exit_status": 0,
                                                "validation_status": "VALID", "metrics": {"avg_departure_delay_minutes": 1.0}}
            manager.start(exp["id"], workers=1)
            manager._futures[exp["id"]].join(5)
            result = manager.get(exp["id"])
            self.assertEqual(result["status"], "COMPLETED")
            archived = list(Path(temp).glob("partial-output.interrupted-*"))
            self.assertEqual(len(archived), 1)
            self.assertEqual((archived[0] / "marker.txt").read_text(encoding="utf-8"), "partial")

    def test_parallel_worker_bound_isolation_failure_recovery_and_resume(self):
        with tempfile.TemporaryDirectory(prefix="ramplab-goal32-manager-") as temp:
            manager = experiments.ExperimentManager(Path(temp), FakeStudio)
            manager._ensure_cli = lambda: Path("fake-airside-cli")
            exp = manager.create(definition())
            self.assertEqual(exp["counts"]["configurations"], 5)
            self.assertEqual(exp["counts"]["total"], 10)
            self.assertLessEqual(exp["execution"]["workers"], experiments.MAX_WORKERS)
            active, high_water = 0, 0
            lock = threading.Lock()
            outputs = []

            def fake_run(run, _scenario, _folder):
                nonlocal active, high_water
                with lock:
                    active += 1; high_water = max(high_water, active); outputs.append(run["output_path"])
                time.sleep(.005)
                with lock: active -= 1
                if run["seed"] == 43 and run["configuration_id"] != next(x["configuration_id"] for x in exp["configurations"] if x["configuration_id"].startswith("baseline_")):
                    return {"status": "FAILED", "exit_status": 7, "validation_status": "FAILED", "message": "controlled unit failure"}
                return {"status": "COMPLETED", "exit_status": 0, "validation_status": "VALID", "metrics": {"avg_departure_delay_minutes": 1.0, "surface_aircraft_aircraft_collisions": 0}}

            manager._execute_run = fake_run
            manager.start(exp["id"], workers=3)
            thread = manager._futures[exp["id"]]; thread.join(5)
            self.assertFalse(thread.is_alive())
            result = manager.get(exp["id"])
            self.assertEqual(result["status"], "COMPLETED_WITH_ERRORS")
            self.assertGreaterEqual(result["counts"]["failed"], 1)
            self.assertGreaterEqual(result["counts"]["completed"], 1)
            self.assertLessEqual(high_water, 3)
            self.assertEqual(len(outputs), len(set(outputs)))
            before = sum(run["status"] == "COMPLETED" for run in result["runs"])
            manager.start(exp["id"], workers=2)
            manager._futures[exp["id"]].join(5)
            resumed = manager.get(exp["id"])
            self.assertGreaterEqual(sum(run["status"] == "COMPLETED" for run in resumed["runs"]), before)

    def test_cache_requires_valid_artifacts_and_run_identity_changes_with_inputs(self):
        with tempfile.TemporaryDirectory(prefix="ramplab-goal32-cache-") as temp:
            root = Path(temp); manager = experiments.ExperimentManager(root, FakeStudio)
            valid = root / "valid"; valid.mkdir(); (valid / "experiment.json").write_text("{}", encoding="utf-8")
            invalid = root / "invalid"; invalid.mkdir()
            import hashlib
            hashes = {}
            item = {"validation_status": "VALID", "result_digest": "abc", "output_path": str(valid), "artifact_hashes": hashes}
            for name in ("experiment.json", "runs.csv", "simulator-metrics.json", "events.jsonl"):
                artifact = valid / name; artifact.write_text("{}", encoding="utf-8")
                hashes[name] = hashlib.sha256(artifact.read_bytes()).hexdigest()
            self.assertTrue(experiments._cached_valid(item))
            (valid / "events.jsonl").write_text("corrupt", encoding="utf-8")
            self.assertFalse(experiments._cached_valid(item))
            self.assertFalse(experiments._cached_valid({"validation_status": "VALID", "result_digest": "abc", "output_path": str(invalid)}))
            a = experiments.digest({"scenario": base_scenario(), "seed": 42, "revision": "x"})
            changed = base_scenario(); changed["seed"] = 43
            b = experiments.digest({"scenario": changed, "seed": 43, "revision": "x"})
            self.assertNotEqual(a, b)


if __name__ == "__main__":
    unittest.main()
