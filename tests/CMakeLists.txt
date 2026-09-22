# Test suite. Tests are plain executables registered with CTest; no timeout is
# configured anywhere: a hanging test is a defect to diagnose, not to hide.
function(fum_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE fum_core fum_synthetic fum_warnings)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

fum_add_test(test_core test_core.cpp)
fum_add_test(test_model test_model.cpp)
fum_add_test(test_engine_lifecycle test_engine_lifecycle.cpp)
fum_add_test(test_engine_faults test_engine_faults.cpp)
fum_add_test(test_store test_store.cpp)

fum_add_test(test_concurrency test_concurrency.cpp)
fum_add_test(test_property test_property.cpp)
fum_add_test(test_adversarial test_adversarial.cpp)

if(FUM_BUILD_LOCAL_PROCESS_ADAPTER)
  add_executable(test_local_process test_local_process.cpp)
  target_link_libraries(test_local_process PRIVATE
    fum_core fum_synthetic fum_local_process fum_warnings)
  target_include_directories(test_local_process PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME test_local_process COMMAND test_local_process)
  # The control plane must be built before its test runs.
  add_dependencies(test_local_process fum_test_control_plane)
endif()
