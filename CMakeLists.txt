cmake_minimum_required(VERSION 3.18)
project(crashvault VERSION 0.1.0 LANGUAGES CXX)

# ── C++ standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # Ensure we use -std=c++17 not -std=gnu++17

# ── Build type ────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ── Compiler warnings — strict per project requirement ───────────────────────
# WHY -Wall -Wextra -Werror: Catch bugs at compile time. The project requirement
# mandates zero warnings.
add_compile_options(
  -Wall -Wextra -Werror
  -Wno-unused-parameter      # some callback params intentionally unused
  -Wno-missing-field-initializers
  -pthread
)

add_link_options(-pthread)

# ── Sanitizer builds ─────────────────────────────────────────────────────────
# Selected via: cmake -DSANITIZE=asan|ubsan|tsan|none
set(SANITIZE "none" CACHE STRING "Sanitizer to enable (asan|ubsan|tsan|none)")
if(SANITIZE STREQUAL "asan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
  message(STATUS "ASan+UBSan enabled")
elseif(SANITIZE STREQUAL "ubsan")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined)
  message(STATUS "UBSan enabled")
elseif(SANITIZE STREQUAL "tsan")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
  message(STATUS "TSan enabled")
endif()

# ── Failpoint support ─────────────────────────────────────────────────────────
# WHY: Failpoints are compiled IN in Debug/RelWithDebInfo builds and
# activated via environment variables at runtime. They are compiled OUT
# in Release builds to have zero overhead in production.
if(CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo")
  add_compile_definitions(CV_FAILPOINTS_ENABLED=1)
  message(STATUS "Failpoints ENABLED (debug build)")
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
# We use CMake's FetchContent for doctest and nlohmann/json (header-only),
# so they don't need to be installed on the system.

include(FetchContent)

# doctest — single-header unit test framework
# We only need the single header file, so we download it directly
# rather than using FetchContent (avoids the cmake_minimum_required compat issue)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
)
set(FETCHCONTENT_QUIET OFF)
# Apply cmake policy to allow older minimum version in doctest
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# nlohmann/json — header-only JSON library (used for IPC protocol)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/vendor
  ${doctest_SOURCE_DIR}              # gives us #include "doctest/doctest.h"
  ${doctest_SOURCE_DIR}/doctest      # also allows #include "doctest.h"
)

# ── Library: daemon core (shared between daemon and tests) ────────────────────
add_library(cv_daemon_core STATIC
  src/daemon/config.cc
  src/daemon/signal_handler.cc
  src/daemon/daemonize.cc
  src/daemon/logger.cc
)
target_link_libraries(cv_daemon_core PUBLIC nlohmann_json::nlohmann_json)

# ── Library: watcher ──────────────────────────────────────────────────────────
add_library(cv_watcher STATIC
  src/watcher/inotify_watcher.cc
  src/watcher/debouncer.cc
)
target_link_libraries(cv_watcher PUBLIC cv_daemon_core)

# ── Library: pipeline ─────────────────────────────────────────────────────────
add_library(cv_pipeline STATIC
  src/pipeline/bounded_queue.cc
  src/pipeline/worker_pool.cc
)
target_link_libraries(cv_pipeline PUBLIC cv_daemon_core)

# ── Library: IPC ──────────────────────────────────────────────────────────────
add_library(cv_ipc STATIC
  src/ipc/socket_server.cc
)
target_link_libraries(cv_ipc PUBLIC cv_daemon_core)

# ── Executable: crashvaultd ───────────────────────────────────────────────────
add_executable(crashvaultd
  src/daemon/main.cc
)
target_link_libraries(crashvaultd PRIVATE
  cv_daemon_core
  cv_watcher
  cv_pipeline
  cv_ipc
  nlohmann_json::nlohmann_json
)

# ── Executable: cvctl (CLI client) ────────────────────────────────────────────
add_executable(cvctl
  src/cli/main.cc
)
target_link_libraries(cvctl PRIVATE
  nlohmann_json::nlohmann_json
)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

# Phase 1 unit tests
add_executable(test_phase1
  tests/unit/test_phase1.cc
)
target_link_libraries(test_phase1 PRIVATE
  cv_daemon_core
  cv_watcher
  cv_pipeline
  doctest::doctest
)
# doctest self-registration: we just run the binary
add_test(NAME unit_phase1 COMMAND test_phase1)

# Integration tests (Python scripts — discovered by CTest)
find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(
    NAME integration_watcher_phase1
    COMMAND ${PYTHON3}
      ${CMAKE_SOURCE_DIR}/tests/integration/test_watcher_phase1.py
      $<TARGET_FILE:crashvaultd>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(integration_watcher_phase1 PROPERTIES TIMEOUT 60)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS crashvaultd cvctl RUNTIME DESTINATION bin)
install(FILES
  ${CMAKE_SOURCE_DIR}/scripts/demo.sh
  DESTINATION share/crashvault
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "CrashVault build configuration:")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Sanitizer:     ${SANITIZE}")
message(STATUS "  C++ standard:  C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Failpoints:    ${CV_FAILPOINTS_ENABLED}")
message(STATUS "")
