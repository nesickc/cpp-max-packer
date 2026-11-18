# libFuzzer targets for the readers of untrusted input (spec 10.1). Configured only
# by the windows-ninja-fuzz preset, which builds everything under AddressSanitizer.
#
# CTest runs each target for SPECTRAPACK_FUZZ_SECONDS on its checked-in corpus with
# a per-input timeout and a memory cap; a crash, a hang or a breach of the cap
# fails the test and leaves the input in out/build/windows-ninja-fuzz/fuzz/<target>/.
set(SPECTRAPACK_FUZZ_SECONDS 60 CACHE STRING "Seconds each fuzz target runs under CTest")

# The instrumented executables load the AddressSanitizer runtime that ships
# beside the compiler.
get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(asan_runtime "${compiler_directory}/clang_rt.asan_dynamic-x86_64.dll")
if(NOT EXISTS "${asan_runtime}")
  message(FATAL_ERROR "AddressSanitizer runtime not found: ${asan_runtime}")
endif()
add_custom_command(OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${asan_runtime}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/"
  DEPENDS "${asan_runtime}")
add_custom_target(spectrapack_fuzzers ALL
  DEPENDS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll")

# spectrapack_fuzz_test(<name> <per-input timeout, s> <memory cap, MiB> <largest input, bytes>)
function(spectrapack_fuzz_test name input_timeout memory_mb max_length)
  set(work "${CMAKE_BINARY_DIR}/fuzz/${name}")
  file(MAKE_DIRECTORY "${work}/corpus")
  add_dependencies(spectrapack_fuzzers fuzz_${name})
  add_test(NAME fuzz_${name}
    COMMAND fuzz_${name}
      -max_total_time=${SPECTRAPACK_FUZZ_SECONDS} -timeout=${input_timeout}
      -rss_limit_mb=${memory_mb} -malloc_limit_mb=${memory_mb} -max_len=${max_length}
      -print_final_stats=1 "-artifact_prefix=${work}/"
      "${work}/corpus" "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
  # Reading the corpus and one last slow input may follow the fuzzing time.
  math(EXPR watchdog "${SPECTRAPACK_FUZZ_SECONDS} + 2 * ${input_timeout} + 120")
  set_tests_properties(fuzz_${name} PROPERTIES TIMEOUT ${watchdog})
endfunction()

foreach(name IN ITEMS stl_import envelope contracts protocol)
  add_executable(fuzz_${name} ${name}.cpp)
  target_compile_features(fuzz_${name} PRIVATE cxx_std_20)
endforeach()
target_link_libraries(fuzz_stl_import PRIVATE pack_geometry)
target_link_libraries(fuzz_envelope PRIVATE pack_geometry)
# The envelope target also calls the build with its own search budget.
target_include_directories(fuzz_envelope PRIVATE "${PROJECT_SOURCE_DIR}/engine/pack_geometry/src")
target_link_libraries(fuzz_contracts PRIVATE pack_io)
target_link_libraries(fuzz_protocol PRIVATE pack_service)

# Project open is the desktop shell's Rust reader. Cargo builds its fuzz entry
# (desktop/src-tauri/fuzz) as a static library with libFuzzer's coverage counters;
# AddressSanitizer covers the unsafe Windows calls and the process heap.
find_program(SPECTRAPACK_CARGO cargo REQUIRED
  DOC "cargo of the toolchain pinned in desktop/src-tauri/rust-toolchain.toml")
set(cargo_directory "${CMAKE_BINARY_DIR}/cargo")
set(project_archive_library
  "${cargo_directory}/x86_64-pc-windows-msvc/release/examples/fuzz_project_archive.lib")
string(JOIN " " project_archive_rustflags
  -Ctarget-feature=+crt-static
  -Cpasses=sancov-module
  -Cllvm-args=-sanitizer-coverage-level=4
  -Cllvm-args=-sanitizer-coverage-inline-8bit-counters
  -Cllvm-args=-sanitizer-coverage-pc-table
  -Cllvm-args=-sanitizer-coverage-trace-compares
  -Coverflow-checks=yes -Cdebug-assertions=yes -Cdebuginfo=1)
# Cargo tracks its own inputs, so it runs on every build; the explicit target
# keeps the flags off build scripts.
add_custom_target(fuzz_project_archive_library
  COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=${project_archive_rustflags}"
    "${SPECTRAPACK_CARGO}" build --locked --no-default-features --release
    --target x86_64-pc-windows-msvc --example fuzz_project_archive --target-dir "${cargo_directory}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/desktop/src-tauri"
  BYPRODUCTS "${project_archive_library}"
  USES_TERMINAL)
add_executable(fuzz_project_archive project_archive.cpp)
add_dependencies(fuzz_project_archive fuzz_project_archive_library)
# Rust's counters come with a PC table and MSVC's do not, and libFuzzer wants one
# kind: the C++ unit is built without coverage and names the driver library itself.
set_property(TARGET fuzz_project_archive PROPERTY COMPILE_OPTIONS /fsanitize=address /Zi)
target_link_libraries(fuzz_project_archive PRIVATE
  "${project_archive_library}" clang_rt.fuzzer_MT-x86_64 ntdll userenv ws2_32 dbghelp)

spectrapack_fuzz_test(stl_import 20 2048 65536)
spectrapack_fuzz_test(envelope 120 2048 4096)
spectrapack_fuzz_test(contracts 20 2048 65536)
spectrapack_fuzz_test(protocol 20 2048 8192)
spectrapack_fuzz_test(project_archive 20 2048 16384)
