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
