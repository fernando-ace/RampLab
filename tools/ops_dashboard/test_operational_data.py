"""Operational schedule, timezone, replay and provenance tests."""
from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

from tools.ops_dashboard import operational_replay, operational_schedule, operational_time

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures" / "operational"
PROFILE = json.loads((FIXTURES / "kauo-demo-schedule-profile.json").read_text(encoding="utf-8"))
CSV_TEXT = (FIXTURES / "kauo-tomorrow-schedule.csv").read_text(encoding="utf-8")
EVENT_TEXT = (FIXTURES / "kauo-synthetic-events.json").read_text(encoding="utf-8")


def imported_dataset(content: str = CSV_TEXT, profile: dict | None = None):
    return operational_schedule.import_schedule("plan.csv", content, deepcopy(profile or PROFILE))


class OperationalTimeTests(unittest.TestCase):
    def test_utc_and_airport_local_conversion(self):
        utc = operational_time.parse_utc("2026-10-12T12:00:00Z", "America/Chicago")
        local = operational_time.parse_utc("2026-10-12 07:00", "America/Chicago", timestamp_format="%Y-%m-%d %H:%M")
        self.assertEqual(utc, local)
        self.assertEqual(operational_time.iso_utc(local), "2026-10-12T12:00:00Z")

    def test_dst_gap_and_fold_require_resolution(self):
        with self.assertRaisesRegex(operational_time.OperationalTimeError, "nonexistent"):
            operational_time.parse_utc("2026-03-08 02:30", "America/Chicago", timestamp_format="%Y-%m-%d %H:%M")
        with self.assertRaisesRegex(operational_time.OperationalTimeError, "ambiguous"):
            operational_time.parse_utc("2026-11-01 01:30", "America/Chicago", timestamp_format="%Y-%m-%d %H:%M")
        early = operational_time.parse_utc("2026-11-01 01:30", "America/Chicago", timestamp_format="%Y-%m-%d %H:%M", fold=0)
        late = operational_time.parse_utc("2026-11-01 01:30", "America/Chicago", timestamp_format="%Y-%m-%d %H:%M", fold=1)
        self.assertEqual((late - early).total_seconds(), 3600)

    def test_missing_timezone_invalid_timestamp_and_explicit_offset(self):
        with self.assertRaisesRegex(operational_time.OperationalTimeError, "timezone"):
            operational_time.parse_utc("2026-10-12 07:00", "")
        with self.assertRaises(operational_time.OperationalTimeError):
            operational_time.parse_utc("tomorrow morning", "America/Chicago")
        self.assertEqual(operational_time.iso_utc(operational_time.parse_utc("2026-10-12T07:00:00-05:00", "UTC")),
                         "2026-10-12T12:00:00Z")


class ScheduleImportTests(unittest.TestCase):
    def test_csv_import_mapping_provenance_quality_and_goal24a_adapter(self):
        data = imported_dataset()
        self.assertEqual(data["airport_id"], "KAUO")
        self.assertEqual(data["timezone"], "America/Chicago")
        self.assertEqual(data["flights"][0]["scheduled_arrival"], "2026-10-12T12:00:00Z")
        self.assertEqual(data["flights"][0]["source"]["row"], 2)
        self.assertEqual(data["provenance"]["source_sha256"], operational_schedule.sha256(CSV_TEXT.encode()))
        self.assertEqual(data["provenance"]["data_classification"], "SYNTHETIC")
        self.assertEqual(data["quality_summary"]["status"], "VALID")
        package = operational_schedule.validate_through_goal24a(data)
        self.assertEqual(len(package["flights"]), 6)
        self.assertEqual(len(package["turnaround_requirements"]), 3)
        self.assertTrue(all(row["service_type"] == "pushback_preparation" for row in package["turnaround_requirements"]))
        self.assertEqual(package["dataset"]["dataset_id"], data["dataset_id"])
        with tempfile.TemporaryDirectory(prefix="ramplab-goal33-package-") as temp:
            package_path = Path(temp) / "canonical.json"
            package_path.write_bytes(operational_schedule.canonical_bytes(package))
            output = Path(temp) / "generated"
            from tools.airport_scenario_generation.generator import generate, validate_generated
            result = generate(package_path, ROOT / "tools" / "airport_scenario_generation" / "mapping.kauo.json", output, seed=42)
            self.assertEqual(len(result["scenario"]["aircraft"]), 3)
            self.assertEqual(len(result["scenario"]["aircraft"][0]["service_tasks"]), 1)
            self.assertTrue(validate_generated(output)["valid"])
            self.assertTrue((output / "scenario.json").is_file())

    def test_json_import_and_suggestion_for_external_headers(self):
        source = json.dumps([{"Flight": "AB7", "Tail": "N7", "Aircraft": "C172", "ETA": "2026-10-12T12:00:00Z", "Gate": "GA1"}])
        result = operational_schedule.preview("schedule.json", source)
        self.assertEqual(result["suggested_fields"]["flight.id"], "Flight")
        profile = deepcopy(PROFILE)
        profile["source_format"] = "json"
        profile["fields"] = {"flight.id": "Flight", "aircraft.id": "Tail", "aircraft.type": "Aircraft",
                              "flight.scheduled_arrival": "ETA", "flight.stand_id": "Gate"}
        dataset = operational_schedule.import_schedule("schedule.json", source, profile)
        self.assertEqual(dataset["flights"][0]["flight_id"], "AB7")

    def test_mapping_profile_rejects_unknown_targets_missing_timezone_and_unknown_stand(self):
        broken = deepcopy(PROFILE)
        broken["fields"]["flight.not_supported"] = "Flight No"
        with self.assertRaisesRegex(operational_schedule.ScheduleImportError, "Unsupported mapping"):
            operational_schedule.validate_profile(broken)
        broken = deepcopy(PROFILE); broken["timezone"] = "Not/A_Timezone"
        with self.assertRaisesRegex(ValueError, "Unknown IANA timezone"):
            operational_schedule.validate_profile(broken)
        broken = deepcopy(PROFILE); del broken["timezone"]
        with self.assertRaisesRegex(operational_schedule.ScheduleImportError, "timezone"):
            operational_schedule.validate_profile(broken)
        dataset = imported_dataset(CSV_TEXT.replace(",GA1,", ",A7,"))
        self.assertEqual(dataset["quality_summary"]["errors"], 3)
        self.assertTrue(all(item["code"] == "STAND_UNKNOWN" for item in dataset["findings"]))

    def test_duplicate_flight_and_bad_timestamps_are_reported_with_rows(self):
        duplicate = CSV_TEXT + CSV_TEXT.splitlines()[1] + "\n"
        data = imported_dataset(duplicate)
        self.assertIn("FLIGHT_ID_DUPLICATE", {item["code"] for item in data["findings"]})
        invalid = imported_dataset(CSV_TEXT.replace("2026-10-12 07:00", "not-a-time", 1))
        finding = next(item for item in invalid["findings"] if item["code"] == "TIMESTAMP_INVALID")
        self.assertEqual(finding["row"], 2)
        self.assertEqual(finding["field"], "flight.scheduled_arrival")

    def test_time_only_cross_midnight_uses_next_local_date(self):
        profile = deepcopy(PROFILE)
        profile["timestamp_format"] = "%H:%M"
        source = "Flight No,Tail,Aircraft,Scheduled Arrival,Scheduled Departure,Gate\nNIGHT1,NIGHT1,C172,23:30,01:00,GA1\n"
        flight = imported_dataset(source, profile)["flights"][0]
        self.assertEqual(flight["scheduled_arrival"], "2026-10-13T04:30:00Z")
        self.assertEqual(flight["scheduled_departure"], "2026-10-13T06:00:00Z")
        self.assertTrue(flight["rollover_applied"])

    def test_stand_overlap_is_a_data_quality_error(self):
        source = "Flight No,Tail,Aircraft,Scheduled Arrival,Scheduled Departure,Gate\nA,N1,C172,2026-10-12 07:00,2026-10-12 08:00,GA1\nB,N2,C172,2026-10-12 07:30,2026-10-12 08:30,GA1\n"
        data = imported_dataset(source)
        self.assertIn("STAND_OCCUPANCY_CONFLICT", {item["code"] for item in data["findings"]})
        with self.assertRaisesRegex(operational_schedule.ScheduleImportError, "overlap"):
            operational_schedule.to_canonical_package(data)

    def test_profile_defaults_are_explicit_and_unknowns_are_not_filled(self):
        source = "Flight No,Tail,Scheduled Arrival,Gate\nA,N1,2026-10-12 07:00,GA1\n"
        data = imported_dataset(source)
        self.assertIsNone(data["flights"][0]["aircraft_type"])
        profile = deepcopy(PROFILE); profile["defaults"] = {"aircraft.type": "C172"}
        filled = imported_dataset(source, profile)
        self.assertEqual(filled["flights"][0]["aircraft_type"], "C172")

    def test_service_assumption_profile_requires_positive_duration(self):
        invalid = deepcopy(PROFILE)
        invalid["scenario_defaults"]["turnaround_services"] = [{"service_type": "pushback_preparation", "duration_minutes": 0}]
        with self.assertRaisesRegex(operational_schedule.ScheduleImportError, "positive duration"):
            operational_schedule.validate_profile(invalid)


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.dataset = imported_dataset()
        self.events = operational_replay.parse_events("events.json", EVENT_TEXT, "America/Chicago")

    def test_historical_events_sort_deterministically_and_deduplicate(self):
        ids = [event["event_id"] for event in self.events["events"]]
        self.assertEqual(ids, sorted(ids, key=lambda ident: next((event["timestamp"], ident) for event in self.events["events"] if event["event_id"] == ident)))
        raw = json.loads(EVENT_TEXT)
        raw["events"].append(deepcopy(raw["events"][0]))
        parsed = operational_replay.parse_events("events.json", json.dumps(raw), "America/Chicago")
        self.assertEqual(parsed["event_count"], self.events["event_count"])
        self.assertEqual(parsed["duplicate_count"], 1)

    def test_idempotency_key_collision_is_rejected_as_quality_error(self):
        source = {"events": [{"event_id": "dup", "event_type": "actual_arrival", "timestamp": "2026-10-12T12:04:00Z", "entity_id": "AX201", "payload": {}},
                             {"event_id": "dup", "event_type": "actual_arrival", "timestamp": "2026-10-12T12:05:00Z", "entity_id": "AX201", "payload": {}}]}
        parsed = operational_replay.parse_events("events.json", json.dumps(source), "UTC")
        self.assertEqual(parsed["event_count"], 1)
        self.assertIn("EVENT_ID_COLLISION", {item["code"] for item in parsed["findings"]})

    def test_replay_seek_step_and_speed_use_virtual_recorded_time(self):
        replay = operational_replay.replay_timeline(self.dataset, self.events["events"])
        clock = operational_replay.ReplayClock(self.dataset, replay)
        first = clock.step()
        self.assertEqual(first["timestamp"], "2026-10-12T12:04:00Z")
        self.assertEqual(first["state"]["flights"]["AX201"]["status"], "ARRIVED")
        playing = clock.play(5)
        self.assertTrue(playing["playing"])
        advanced = clock.advance(60)
        self.assertTrue(advanced["playing"])
        self.assertEqual(advanced["timestamp"], "2026-10-12T12:09:00Z")
        paused = clock.pause()
        self.assertFalse(paused["playing"])
        sought = clock.seek("2026-10-12T12:25:00Z")
        self.assertEqual(sought["state"]["flights"]["AX201"]["status"], "SERVICE_COMPLETE")

    def test_out_of_order_input_and_repeated_replay_have_stable_digest(self):
        forward = self.events["events"]
        reverse = list(reversed(forward))
        left = operational_replay.replay_timeline(self.dataset, forward)
        right = operational_replay.replay_timeline(self.dataset, reverse)
        self.assertEqual(left["normalized_sha256"], right["normalized_sha256"])
        self.assertEqual(left["final_state"], right["final_state"])

    def test_plan_vs_actual_distinguishes_observed_and_unknown(self):
        result = operational_schedule.plan_vs_actual(self.dataset, self.events["events"])
        first = next(row for row in result if row["flight_id"] == "AX201")
        second = next(row for row in result if row["flight_id"] == "AX203")
        gate_change = next(row for row in result if row["flight_id"] == "AX202")
        self.assertEqual(first["arrival_variance_minutes"], 4)
        self.assertEqual(first["departure_variance_minutes"], 8)
        self.assertEqual(first["evidence"], "OBSERVED")
        self.assertIsNone(second["departure_variance_minutes"])
        self.assertIsNone(second["stand_actual"])
        self.assertFalse(gate_change["stand_variance"])

    def test_live_freshness_states_are_explicit(self):
        self.assertEqual(operational_replay.freshness([])["status"], "UNKNOWN")
        future = {"timestamp": "2026-10-12T12:00:00Z", "received_at": "2026-10-12T12:00:00Z"}
        self.assertEqual(operational_replay.freshness([future], now="2026-10-11T12:00:00Z")["status"], "FUTURE")
        event = {"timestamp": "2026-10-12T12:00:00Z", "received_at": "2026-10-12T12:00:00Z"}
        self.assertEqual(operational_replay.freshness([event], now="2026-10-12T12:04:00Z")["status"], "CURRENT")
        self.assertEqual(operational_replay.freshness([event], now="2026-10-12T12:10:00Z")["status"], "DELAYED")
        self.assertEqual(operational_replay.freshness([event], now="2026-10-12T12:20:00Z")["status"], "STALE")


if __name__ == "__main__":
    unittest.main()
