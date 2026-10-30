# Catch names are available when CTest includes this file, after discovery.
# These finite full-asset cases exceed the runner's ordinary 30-second watchdog:
# the retained successful Debug Ulamok/Pryanik pair took 548 seconds, and a
# single checked-export phase exceeded 33 seconds. This 1200-second allowance
# is a qualification watchdog, not a performance target or relaxed work cap.
set(qualification_discovered
  ${spectrapack_import_tests_TESTS}
  ${spectrapack_conservative_fields_tests_TESTS}
  ${spectrapack_solver_tests_TESTS})
foreach(qualification_test IN ITEMS
    "T011 full Pryanik preparation fits actual session reserve and pinned source"
    "T010 Ulamok full cube field pass preserves 36"
    "T010 full Pryanik field profiles preserve two native-valid copies"
    "T010 full36 independently quantized Ulamok export fits unchanged caps"
    "T010 full Pryanik retained2 quantized export preserves native found copies"
    "T010 actual first quantized Pryanik2 copy completes within diagnostic half-aggregate work")
  list(FIND qualification_discovered "${qualification_test}" qualification_index)
  if(qualification_index GREATER -1)
    set_tests_properties("${qualification_test}" PROPERTIES TIMEOUT 1200)
  endif()
endforeach()
unset(qualification_test)

# The finite 468-rasterization matrix completed in 39.03 seconds on the held
# Debug binary. Allow 120 seconds for host variance; its work assertions stand.
list(FIND qualification_discovered
  "T010 object admission encloses every catalog window and translated axis" qualification_index)
if(qualification_index GREATER -1)
  set_tests_properties(
    "T010 object admission encloses every catalog window and translated axis"
    PROPERTIES TIMEOUT 120)
endif()
unset(qualification_index)
unset(qualification_discovered)

# This public-boundary test generates and replays an envelope before checking
# three tampered reports. Debug took 114 seconds with shared setup; allow host variance.
list(FIND spectrapack_result_builder_tests_TESTS
  "Packing envelope replay binds source and parameters" envelope_replay_index)
if(envelope_replay_index GREATER -1)
  set_tests_properties("Packing envelope replay binds source and parameters" PROPERTIES TIMEOUT 180)
endif()
unset(envelope_replay_index)

# Both source preparation modes generate and replay a certified envelope.
# Debug geometry exceeds the ordinary 30-second watchdog.
list(FIND spectrapack_import_tests_TESTS
  "Automatic packing envelope retains source dimensions and explicit acceptance" envelope_import_index)
if(envelope_import_index GREATER -1)
  set_tests_properties("Automatic packing envelope retains source dimensions and explicit acceptance"
    PROPERTIES TIMEOUT 180)
endif()
unset(envelope_import_index)

# Gap filling rasterizes every cube rotation at 0.1 mm; hosted Debug exceeds 60 seconds.
list(FIND spectrapack_solver_tests_TESTS
  "Gap filling keeps asymmetric proposals clear across cube rotations" gap_fill_index)
if(gap_fill_index GREATER -1)
  set_tests_properties("Gap filling keeps asymmetric proposals clear across cube rotations"
    PROPERTIES TIMEOUT 180)
endif()
unset(gap_fill_index)
