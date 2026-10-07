# RampLab Engineering Evidence Bundle

## Experiments

| Name | Scenario | Seeds | Runs | Completion | Safety |
|---|---|---:|---:|---|---|
| turnaround_flight_bank | scenarios/turnaround_flight_bank.yaml | 42 | 1 | COMPLETE | NO RECORDED COLLISIONS |
| turnaround_flight_bank | scenarios/turnaround_flight_bank_outage.yaml | 42 | 1 | COMPLETE | NO RECORDED COLLISIONS |

## KPI summary

| KPI | control | disruption | Absolute change | Percent change |
|---|---|---|---|---|
| Aircraft count (aircraft_count) | 3.000 count | 3.000 count | 0.000 count | 0.00% |
| Arrival runway wait (arrival_runway_wait_seconds) | 158.000 s | 192.000 s | 34.000 s | 21.52% |
| Arrival taxi distance (arrival_taxi_distance_m) | 326.000 m | 463.000 m | 137.000 m | 42.02% |
| Arrival taxi time (arrival_taxi_seconds) | 326.000 s | 463.000 s | 137.000 s | 42.02% |
| Average runway wait (average_runway_wait_seconds) | 31.600 s | 42.800 s | 11.200 s | 35.44% |
| Mean departure delay (avg_departure_delay_minutes) | 2.100 min | 4.500 min | 2.400 min | 114.29% |
| Mean service waiting (avg_service_waiting_minutes) | 0.800 min | 2.300 min | 1.500 min | 187.50% |
| Mean turnaround (avg_turnaround_minutes) | 24.700 min | 29.100 min | 4.400 min | 17.81% |
| Completed turnarounds (completed_turnarounds) | 3.000 count | 3.000 count | 0.000 count | 0.00% |
| Delayed aircraft (delayed_aircraft) | 1.000 count | 2.000 count | 1.000 count | 100.00% |
| Delayed turnarounds (delayed_turnarounds) | 1.000 count | 2.000 count | 1.000 count | 100.00% |
| Departure runway wait (departure_runway_wait_seconds) | 0.000 s | 22.000 s | 22.000 s | N/A (zero baseline) |
| Departure taxi distance (departure_taxi_distance_m) | 406.000 m | 472.000 m | 66.000 m | 16.26% |
| Departure taxi time (departure_taxi_seconds) | 409.000 s | 476.000 s | 67.000 s | 16.38% |
| Disruption replans (disruption_triggered_replans) | 0.000 count | 1.000 count | 1.000 count | N/A (zero baseline) |
| Failed or timed out turnarounds (failed_or_timed_out_turnarounds) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Fleet collisions (fleet_collisions) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Fleet minimum separation (fleet_minimum_separation_m) | 8.000 m | 2.111 m | -5.889 m | -73.61% |
| Outstanding reservations (fleet_outstanding_reservations) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Fleet reassignments (fleet_reassignments) | 0.000 count | 1.000 count | 1.000 count | N/A (zero baseline) |
| Fleet requests completed (fleet_requests_completed) | 9.000 count | 9.000 count | 0.000 count | 0.00% |
| Fleet requests created (fleet_requests_created) | 9.000 count | 9.000 count | 0.000 count | 0.00% |
| Fleet requests failed (fleet_requests_failed) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Fleet reservation contentions (fleet_reservation_contentions) | 2.000 count | 4.000 count | 2.000 count | 100.00% |
| Unfinished fleet requests (fleet_unfinished_requests) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Peak taxiing aircraft (max_simultaneous_taxiing_aircraft) | 1.000 aircraft | 2.000 aircraft | 1.000 aircraft | 100.00% |
| Maximum departure delay (maximum_departure_delay_seconds) | 126.000 s | 276.000 s | 150.000 s | 119.05% |
| Maximum runway queue (maximum_runway_queue_depth) | 1.000 aircraft | 2.000 aircraft | 1.000 aircraft | 100.00% |
| Maximum service task wait (maximum_service_task_wait_seconds) | 24.000 s | 74.000 s | 50.000 s | 208.33% |
| Maximum turnaround (maximum_turnaround_seconds) | 1,482.000 s | 1,746.000 s | 264.000 s | 17.81% |
| Mean departure delay (mean_departure_delay_seconds) | 126.000 s | 270.000 s | 144.000 s | 114.29% |
| Mean turnaround (mean_turnaround_duration_seconds) | 1,482.000 s | 1,746.000 s | 264.000 s | 17.81% |
| Minimum aircraft-ground separation (minimum_aircraft_ground_separation_m) | 80.083 m | 80.083 m | 0.000 m | 0.00% |
| Minimum aircraft separation (minimum_aircraft_separation_m) | 30.000 m | 27.600 m | -2.400 m | -8.00% |
| On-time departure rate (on_time_departure_rate) | 0.667 ratio | 0.333 ratio | -0.334 ratio | -50.07% |
| Runway operations completed (runway_operations_completed) | 5.000 count | 5.000 count | 0.000 count | 0.00% |
| Runway queueing (runway_queue_seconds) | 10.000 s | 44.000 s | 34.000 s | 340.00% |
| Runway utilization (runway_utilization) | 0.629 ratio | 0.602 ratio | -0.027 ratio | -4.37% |
| Simulation duration (simulated_duration_seconds) | 1,668.000 s | 1,741.000 s | 73.000 s | 4.38% |
| Aircraft collisions (surface_aircraft_aircraft_collisions) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Aircraft-ground collisions (surface_aircraft_ground_collisions) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Completed arrivals (surface_arrived_aircraft) | 2.000 count | 2.000 count | 0.000 count | 0.00% |
| Completed departures (surface_departed_aircraft) | 3.000 count | 3.000 count | 0.000 count | 0.00% |
| Surface reroutes (surface_reroutes) | 0.000 count | 1.000 count | 1.000 count | N/A (zero baseline) |
| Surface safety failures (surface_safe_failures) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |
| Total taxi distance (surface_taxi_distance_m) | 672.000 m | 935.000 m | 263.000 m | 39.14% |
| Total taxi time (surface_taxi_seconds) | 735.000 s | 939.000 s | 204.000 s | 27.76% |
| Surface aircraft (surface_total_aircraft) | 3.000 count | 3.000 count | 0.000 count | 0.00% |
| Surface wait events (surface_wait_events) | 2.000 count | 5.000 count | 3.000 count | 150.00% |
| Surface queueing (surface_wait_seconds) | 95.000 s | 181.000 s | 86.000 s | 90.53% |
| Task reassignments (task_reassignments) | 0.000 count | 1.000 count | 1.000 count | N/A (zero baseline) |
| Service task queueing (total_service_task_wait_seconds) | 48.000 s | 138.000 s | 90.000 s | 187.50% |
| Total turnarounds (total_turnarounds) | 3.000 count | 3.000 count | 0.000 count | 0.00% |
| Unresolved service requests (unresolved_service_requests) | 0.000 count | 0.000 count | 0.000 count | N/A (zero baseline) |

## Safety and determinism

Safety: control: NO RECORDED COLLISIONS; disruption: NO RECORDED COLLISIONS

Determinism: INSUFFICIENT DATA — No independent repeat-run pair was supplied; scenario comparison is not determinism evidence.

## Validation

Overall: **WARNING**

| Check | Status | Detail |
|---|---|---|
| run_count_consistency | PASS | Observed 1 run rows. |
| seed_consistency | PASS | JSON=['42'], CSV=['42'], declared=['42']. |
| aircraft_totals | PASS | Aircraft rows 3; KPI 3. |
| collision_evidence | PASS | Collision KPI(s): fleet_collisions, surface_aircraft_aircraft_collisions, surface_aircraft_ground_collisions |
| minimum_separation_evidence | PASS | Minimum separation KPI present. |
| determinism_evidence | INSUFFICIENT DATA | One experiment does not establish repeat-run determinism. |
| event_file_parseability | PASS | Parsed 5 ordered events. |
| json_readability | PASS | Parsed experiment.json. |
| scenario_identity | PASS | JSON source scenario scenarios/turnaround_flight_bank.yaml; CSV scenario values ['turnaround_flight_bank']. |
| run_count_consistency | PASS | Observed 1 run rows. |
| seed_consistency | PASS | JSON=['42'], CSV=['42'], declared=['42']. |
| aircraft_totals | PASS | Aircraft rows 3; KPI 3. |
| collision_evidence | PASS | Collision KPI(s): fleet_collisions, surface_aircraft_aircraft_collisions, surface_aircraft_ground_collisions |
| minimum_separation_evidence | PASS | Minimum separation KPI present. |
| determinism_evidence | INSUFFICIENT DATA | One experiment does not establish repeat-run determinism. |
| event_file_parseability | PASS | Parsed 6 ordered events. |
| json_readability | PASS | Parsed experiment.json. |
| scenario_identity | PASS | JSON source scenario scenarios/turnaround_flight_bank_outage.yaml; CSV scenario values ['turnaround_flight_bank_outage']. |
| scenario_identity | WARNING | Scenarios differ: scenarios/turnaround_flight_bank.yaml vs scenarios/turnaround_flight_bank_outage.yaml. |

## Recorded disruptions

- disruption event line 2 (sequence 2): turnaround_vehicle_unavailable
- disruption event line 3 (sequence 3): turnaround_task_reassigned
- disruption event line 4 (sequence 4): surface_rerouted

## Event timeline

- control line 1 (sequence 1, t=0 s): aircraft_arrived
- control line 2 (sequence 2, t=158 s): arrival_at_gate
- control line 3 (sequence 3, t=210 s): turnaround_task_completed
- control line 4 (sequence 4, t=480 s): runway_occupied
- control line 5 (sequence 5, t=735 s): aircraft_departed
- disruption line 1 (sequence 1, t=0 s): aircraft_arrived
- disruption line 2 (sequence 2, t=130 s): turnaround_vehicle_unavailable
- disruption line 3 (sequence 3, t=130 s): turnaround_task_reassigned
- disruption line 4 (sequence 4, t=210 s): surface_rerouted
- disruption line 5 (sequence 5, t=408 s): turnaround_task_completed
- disruption line 6 (sequence 6, t=939 s): aircraft_departed

## Evidence files

Source artifacts are copied byte-for-byte under evidence/. SHA-256 hashes are in manifest.json. See kpis.json and validation.json.

## Limitations

Missing values remain unavailable. Collision-free values describe recorded observations only. Determinism is not established by one export or by comparing different scenarios.
