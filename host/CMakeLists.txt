# Host build: every pure (hardware-independent) component, its GoogleTest unit tests, and the
# simulator. No ESP-IDF, no device. See docs/TEST_PLAN.md.
cmake_minimum_required(VERSION 3.25)
project(quartz_host LANGUAGES C CXX)

set(QZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Repository root")
cmake_path(NORMAL_PATH QZ_ROOT)

option(QZ_SANITIZE "Build host code with ASan + UBSan" ON)
option(QZ_COVERAGE "Build host code with gcov instrumentation" OFF)

include(${QZ_ROOT}/cmake/qz_component.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/components.cmake)  # defines QZ_HOST_COMPONENTS

set(CMAKE_CXX_STANDARD ${QZ_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(QZ_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(QZ_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
endif()

# --- GoogleTest (pinned to an immutable commit; v1.18.0) -------------------------------------
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG 063de7e9578f82b369302001269680b4b1553359)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# --- cJSON (host side of the cjson dependency; same upstream version as espressif/cjson) -----
# Added by the weather work package (FetchContent, pinned URL + hash) -> target `cjson`.

# --- Components ---------------------------------------------------------------------------------
function(qz_host_component_tests name)
  file(GLOB _tests CONFIGURE_DEPENDS "${QZ_ROOT}/components/${name}/test/*_test.cpp")
  if(NOT _tests)
    return()
  endif()
  add_executable(${name}_test ${_tests})
  target_link_libraries(${name}_test PRIVATE ${name} qz_testkit GTest::gtest_main GTest::gmock)
  target_compile_features(${name}_test PRIVATE cxx_std_${QZ_CXX_STANDARD})
  target_compile_options(${name}_test PRIVATE ${QZ_WARNING_FLAGS})
  target_compile_definitions(${name}_test PRIVATE
    QZ_TEST_DATA_DIR="${QZ_ROOT}/components/${name}/test/data"
    QZ_GOLDEN_DIR="${QZ_ROOT}/components/qz_selftest/golden")
  gtest_discover_tests(${name}_test DISCOVERY_MODE PRE_TEST)
endfunction()

foreach(_c IN LISTS QZ_HOST_COMPONENTS)
  add_subdirectory(${QZ_ROOT}/components/${_c} ${CMAKE_BINARY_DIR}/components/${_c})
endforeach()
foreach(_c IN LISTS QZ_HOST_COMPONENTS)
  qz_host_component_tests(${_c})
endforeach()

# Simulator and host-only tools (added by the UI/simulator work package).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/sim/CMakeLists.txt)
  add_subdirectory(sim)
endif()
