cmake_minimum_required(VERSION 3.25)

project(SpectraPack VERSION 0.0.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")
# Most executables are tests or test helpers; production targets are grouped below.
set(CMAKE_FOLDER "Tests")

include(CTest)
get_property(ctest_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
if(ctest_targets)
  set_target_properties(${ctest_targets} PROPERTIES FOLDER "CMake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  set(SPECTRAPACK_REQUIRED_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND
     NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL SPECTRAPACK_REQUIRED_MSVC_RUNTIME)
    message(FATAL_ERROR
      "CMAKE_MSVC_RUNTIME_LIBRARY must be ${SPECTRAPACK_REQUIRED_MSVC_RUNTIME} for the static CPU build.")
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "${SPECTRAPACK_REQUIRED_MSVC_RUNTIME}" CACHE STRING
    "SpectraPack's required static MSVC runtime")
endif()

option(SPECTRAPACK_ENABLE_VULKAN "Enable the Vulkan compute backend" OFF)
if(SPECTRAPACK_ENABLE_VULKAN)
  message(FATAL_ERROR
    "SPECTRAPACK_ENABLE_VULKAN=ON is unsupported: the Vulkan compute backend is not implemented.")
endif()

find_package(Catch2 3.11.0 CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator 2.3.0 CONFIG REQUIRED)
find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE SPECTRAPACK_ENGINE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE SPECTRAPACK_GIT_RESULT ERROR_QUIET)
if(NOT SPECTRAPACK_GIT_RESULT EQUAL 0)
  set(SPECTRAPACK_ENGINE_COMMIT "unavailable")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=normal
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE SPECTRAPACK_GIT_CHANGES OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE SPECTRAPACK_GIT_STATUS_RESULT ERROR_QUIET)
  if(NOT SPECTRAPACK_GIT_STATUS_RESULT EQUAL 0)
    set(SPECTRAPACK_ENGINE_COMMIT "unavailable")
  elseif(NOT SPECTRAPACK_GIT_CHANGES STREQUAL "")
    string(APPEND SPECTRAPACK_ENGINE_COMMIT "-dirty")
  endif()
endif()
set(SPECTRAPACK_ENGINE_VERSION "${PROJECT_VERSION}")

add_subdirectory(engine/dependencies)
add_library(spectrapack_runtime_values INTERFACE)
target_include_directories(spectrapack_runtime_values INTERFACE engine/common/include)
target_compile_features(spectrapack_runtime_values INTERFACE cxx_std_20)
add_subdirectory(engine/pack_geometry)
add_subdirectory(engine/pack_compute)
add_subdirectory(engine/pack_solver)
add_subdirectory(engine/pack_io)
add_subdirectory(engine/service)
add_subdirectory(engine/cli)
add_subdirectory(tests)
add_subdirectory(benchmarks)

set_target_properties(pack_geometry pack_compute pack_solver pack_io pack_service
  PROPERTIES FOLDER "Libs")
set_target_properties(spectrapack-engine PROPERTIES FOLDER "SpectraPack")
set_target_properties(spectrapack_service_replay_perf PROPERTIES FOLDER "Benchmarks")
