cmake_minimum_required(VERSION 3.25)

project(FabricUpgradeManager
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral Fabric OS upgrade intent and execution lifecycle runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Upgrade-Manager"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FUM_BUILD_TESTS "Build the Fabric Upgrade Manager test suite" ON)
option(FUM_BUILD_EXAMPLES "Build the Fabric Upgrade Manager examples" ON)
option(FUM_BUILD_BENCHMARKS "Build the Fabric Upgrade Manager benchmarks" ON)
option(FUM_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(FUM_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FUM_BUILD_LOCAL_PROCESS_ADAPTER "Build the real local-process upgrade adapter" ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration - first-party code must compile warning-free
# ---------------------------------------------------------------------------
add_library(fum_warnings INTERFACE)
if(MSVC)
  target_compile_options(fum_warnings INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /utf-8 /EHsc
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  if(FUM_WARNINGS_AS_ERRORS)
    target_compile_options(fum_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(fum_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  if(FUM_ENABLE_ASAN)
    target_compile_options(fum_warnings INTERFACE /fsanitize=address)
    target_compile_definitions(fum_warnings INTERFACE
      _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    target_link_options(fum_warnings INTERFACE /INCREMENTAL:NO)
  endif()
else()
  target_compile_options(fum_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
    -Wformat=2 -Wundef -Wcast-align -Wuseless-cast -Wzero-as-null-pointer-constant)
  if(FUM_WARNINGS_AS_ERRORS)
    target_compile_options(fum_warnings INTERFACE -Werror)
  endif()
  if(FUM_ENABLE_ASAN)
    target_compile_options(fum_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fum_warnings INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core runtime library
# ---------------------------------------------------------------------------
set(FUM_CORE_SOURCES
  src/core/crc.cpp
  src/core/result.cpp
  src/core/fs.cpp
  src/core/hash.cpp
  src/core/ids.cpp
  src/core/json.cpp
  src/core/log.cpp
  src/core/time.cpp
  src/model/artifact.cpp
  src/model/campaign.cpp
  src/model/decision.cpp
  src/model/evidence.cpp
  src/model/lifecycle.cpp
  src/model/policy.cpp
  src/model/provenance.cpp
  src/model/skew.cpp
  src/model/target.cpp
  src/model/version.cpp
  src/ports/ports.cpp
  src/store/journal.cpp
  src/store/state.cpp
  src/runtime/worker_pool.cpp
  src/engine/engine.cpp
  src/engine/engine_exec.cpp
  src/engine/engine_ops.cpp
  src/engine/engine_run.cpp
  src/engine/planner.cpp
  src/engine/preflight.cpp
  src/engine/reconcile.cpp
  src/engine/verification.cpp
)

add_library(fum_core STATIC ${FUM_CORE_SOURCES})
add_library(fum::core ALIAS fum_core)
set_target_properties(fum_core PROPERTIES EXPORT_NAME core)
target_include_directories(fum_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fum_core PUBLIC Threads::Threads PRIVATE fum_warnings)
target_compile_features(fum_core PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# Synthetic adapters (deterministic, cross-platform proof surface)
# ---------------------------------------------------------------------------
add_library(fum_synthetic STATIC src/adapters/synthetic_adapter.cpp)
add_library(fum::synthetic ALIAS fum_synthetic)
set_target_properties(fum_synthetic PROPERTIES EXPORT_NAME synthetic)
target_link_libraries(fum_synthetic PUBLIC fum_core PRIVATE fum_warnings)

# ---------------------------------------------------------------------------
# Real local-process upgrade adapter
# ---------------------------------------------------------------------------
if(FUM_BUILD_LOCAL_PROCESS_ADAPTER)
  add_library(fum_local_process STATIC
    src/adapters/local_process/framed_transport.cpp
    src/adapters/local_process/local_process_adapter.cpp
    src/adapters/local_process/process_supervisor.cpp)
  add_library(fum::local_process ALIAS fum_local_process)
  set_target_properties(fum_local_process PROPERTIES EXPORT_NAME local_process)
  target_include_directories(fum_local_process PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/local_process")
  target_link_libraries(fum_local_process PUBLIC fum_core PRIVATE fum_warnings)
  if(WIN32)
    target_link_libraries(fum_local_process PRIVATE ws2_32)
  endif()

  # A real control-plane executable the adapter installs and restarts.
  add_executable(fum_test_control_plane src/adapters/local_process/test_control_plane_main.cpp)
  target_include_directories(fum_test_control_plane PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/local_process")
  target_link_libraries(fum_test_control_plane PRIVATE fum_core fum_local_process fum_warnings)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(fum_cli
  src/cli/main.cpp
  src/cli/commands.cpp
  src/cli/runtime_assembly.cpp)
set_target_properties(fum_cli PROPERTIES OUTPUT_NAME fum)
target_include_directories(fum_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cli")
target_link_libraries(fum_cli PRIVATE fum_core fum_synthetic fum_warnings)
if(FUM_BUILD_LOCAL_PROCESS_ADAPTER)
  target_link_libraries(fum_cli PRIVATE fum_local_process)
  target_compile_definitions(fum_cli PRIVATE FUM_CLI_HAS_LOCAL_PROCESS=1)
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------
if(FUM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(FUM_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(FUM_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fum_warnings EXPORT fumTargets)
install(TARGETS fum_core fum_synthetic
  EXPORT fumTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(FUM_BUILD_LOCAL_PROCESS_ADAPTER)
  install(TARGETS fum_local_process
    EXPORT fumTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS fum_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT fumTargets
  FILE FabricUpgradeManagerTargets.cmake
  NAMESPACE fum::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricUpgradeManager)

configure_package_config_file(
  cmake/FabricUpgradeManagerConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/FabricUpgradeManagerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricUpgradeManager)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FabricUpgradeManagerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FabricUpgradeManagerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricUpgradeManagerConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricUpgradeManager)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/FabricUpgradeManager)
