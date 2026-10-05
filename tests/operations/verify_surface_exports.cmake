if(NOT DEFINED AIRSIDE_CLI OR NOT DEFINED AIRSIDE_SOURCE_DIR OR NOT DEFINED AIRSIDE_OUTPUT_DIR)
    message(FATAL_ERROR "Surface export test requires CLI, source, and output paths")
endif()
file(MAKE_DIRECTORY "${AIRSIDE_OUTPUT_DIR}")

set(control_scenario "${AIRSIDE_SOURCE_DIR}/scenarios/surface_traffic.yaml")
set(disrupted_scenario "${AIRSIDE_SOURCE_DIR}/scenarios/surface_traffic_disrupted.yaml")
set(control_json "${AIRSIDE_OUTPUT_DIR}/control.json")
set(control_csv "${AIRSIDE_OUTPUT_DIR}/control.csv")
set(control_events "${AIRSIDE_OUTPUT_DIR}/control.jsonl")
set(repeat_events "${AIRSIDE_OUTPUT_DIR}/repeat.jsonl")
set(disrupted_json "${AIRSIDE_OUTPUT_DIR}/disrupted.json")

execute_process(COMMAND "${AIRSIDE_CLI}" --scenario "${control_scenario}" --seed 42
    --metrics-json "${control_json}" --metrics-csv "${control_csv}" --record-events "${control_events}" --quiet
    RESULT_VARIABLE control_result ERROR_VARIABLE control_error)
if(NOT control_result EQUAL 0)
    message(FATAL_ERROR "Control CLI failed: ${control_error}")
endif()
execute_process(COMMAND "${AIRSIDE_CLI}" --scenario "${control_scenario}" --seed 42
    --record-events "${repeat_events}" --quiet RESULT_VARIABLE repeat_result ERROR_VARIABLE repeat_error)
if(NOT repeat_result EQUAL 0)
    message(FATAL_ERROR "Repeated control CLI failed: ${repeat_error}")
endif()
execute_process(COMMAND "${AIRSIDE_CLI}" --scenario "${disrupted_scenario}" --seed 42
    --metrics-json "${disrupted_json}" --quiet RESULT_VARIABLE disrupted_result ERROR_VARIABLE disrupted_error)
if(NOT disrupted_result EQUAL 0)
    message(FATAL_ERROR "Disrupted CLI failed: ${disrupted_error}")
endif()

file(READ "${control_json}" control_data)
string(JSON control_departed GET "${control_data}" surface departed_aircraft)
string(JSON control_total GET "${control_data}" surface total_aircraft)
string(JSON throughput GET "${control_data}" surface departure_throughput_per_hour)
string(JSON collisions_aa GET "${control_data}" surface aircraft_aircraft_collisions)
string(JSON collisions_ag GET "${control_data}" surface aircraft_ground_collisions)
if(NOT control_departed EQUAL 3 OR NOT control_total EQUAL 3 OR NOT throughput GREATER 0 OR
   NOT collisions_aa EQUAL 0 OR NOT collisions_ag EQUAL 0)
    message(FATAL_ERROR "Control JSON surface metrics are incomplete or unexpected")
endif()

file(STRINGS "${control_csv}" csv_lines)
list(GET csv_lines 0 csv_header)
foreach(field IN ITEMS surface_departure_throughput_per_hour surface_aircraft_aircraft_collisions
        surface_aircraft_ground_collisions minimum_aircraft_separation_m minimum_aircraft_ground_separation_m
        pushback_started_at_seconds pushback_completed_at_seconds taxi_started_at_seconds
        taxi_completed_at_seconds runway_queue_entered_at_seconds)
    string(FIND "${csv_header}" "${field}" field_index)
    if(field_index EQUAL -1)
        message(FATAL_ERROR "Control CSV is missing ${field}")
    endif()
endforeach()

file(READ "${control_events}" control_event_text)
file(READ "${repeat_events}" repeat_event_text)
if(NOT control_event_text STREQUAL repeat_event_text)
    message(FATAL_ERROR "Repeated seed-42 JSONL event output differs")
endif()
file(STRINGS "${control_events}" event_lines)
set(previous_sequence -1)
set(event_types "")
foreach(event_line IN LISTS event_lines)
    string(JSON sequence GET "${event_line}" sequence)
    string(JSON event_type GET "${event_line}" type)
    if(sequence LESS_EQUAL previous_sequence)
        message(FATAL_ERROR "JSONL sequence is not strictly increasing")
    endif()
    set(previous_sequence "${sequence}")
    list(APPEND event_types "${event_type}")
endforeach()
foreach(required_event IN ITEMS SurfacePushbackStarted SurfacePushbackCompleted SurfaceTaxiRouteAssigned
        SurfaceReservationAcquired SurfaceReservationReleased SurfaceRunwayQueueEntered
        SurfaceRunwayClearance AircraftDeparted)
    if(NOT required_event IN_LIST event_types)
        message(FATAL_ERROR "JSONL is missing ${required_event}")
    endif()
endforeach()

file(READ "${disrupted_json}" disrupted_data)
string(JSON reroutes GET "${disrupted_data}" surface reroutes)
string(JSON safe_failures GET "${disrupted_data}" surface safe_failures)
if(reroutes LESS 1 OR NOT safe_failures EQUAL 0)
    message(FATAL_ERROR "Disrupted JSON does not record a successful reroute")
endif()
