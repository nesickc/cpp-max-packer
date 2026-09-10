cmake_minimum_required(VERSION 3.25)

project(SpectraPack VERSION 0.0.0 LANGUAGES CXX)

include(CTest)

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
add_subdirectory(engine/pack_io)
add_subdirectory(engine/service)
add_subdirectory(engine/cli)
add_subdirectory(tests)
add_subdirectory(benchmarks)
