import unittest

from tools.goal41.decisions import summarize_decisions


class TurnaroundDisruptionSummaryTests(unittest.TestCase):
    def test_pushback_delay_is_explained_from_structured_event_and_scenario(self):
        scenario = {
            "aircraft": [{"id": "KAUO-G31-GA-03", "service_tasks": [
                {"id": "G31-03-PUSH", "type": "Pushback", "duration_seconds": 180}]}],
            "turnaround_disruptions": [{"aircraft": "KAUO-G31-GA-03", "task": "G31-03-PUSH",
                                         "duration_seconds": 480}],
        }
        event = {"sequence": 55, "time_seconds": 241, "type": "TurnaroundDisruptionDetected",
                 "aircraft_id": 3, "aircraft": "KAUO-G31-GA-03", "task_id": 31,
                 "service": "Pushback", "task_duration_seconds": 480}
        rows = summarize_decisions({}, [event], scenario)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["action"], "TurnaroundScheduleAdjusted")
        self.assertEqual(rows[0]["time_seconds"], 241)
        self.assertIn("extended by 300 seconds", rows[0]["what_changed"])
        self.assertIn("readiness", rows[0]["why"])


if __name__ == "__main__":
    unittest.main()
