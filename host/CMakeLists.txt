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
  # vptr is off: Quartz libraries are built -fno-rtti (their vtables carry no typeinfo) while
  # GoogleTest and the test sources use RTTI, and UBSan's vptr check cannot work across that
  # boundary (undefined `typeinfo for <Fake>` at link time, "invalid vptr" at run time).
  add_compile_options(-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
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

# --- cJSON (host side of the cjson dependency; v1.7.19, same upstream version as the device's
# espressif/cjson ==1.7.19~2 managed component). Pinned to an immutable commit. -> target `cjson`
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3)
# cJSON still declares cmake_minimum_required(3.0), which CMake 4 rejects without this floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(cjson)
unset(CMAKE_POLICY_VERSION_MINIMUM)
if(NOT TARGET cjson)
  message(FATAL_ERROR "cJSON did not define the 'cjson' target")
endif()
target_include_directories(cjson PUBLIC ${cjson_SOURCE_DIR})

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

# --- Header self-containment: every public header must compile on its own, under the strict flags
file(GLOB_RECURSE _qz_public_headers CONFIGURE_DEPENDS "${QZ_ROOT}/components/*/include/*.hpp")
set(_qz_header_check_srcs "")
foreach(_h IN LISTS _qz_public_headers)
  cmake_path(RELATIVE_PATH _h BASE_DIRECTORY ${QZ_ROOT}/components OUTPUT_VARIABLE _rel)
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" _id "${_rel}")
  set(_src "${CMAKE_BINARY_DIR}/header_check/${_id}.cpp")
  string(REGEX REPLACE "^[^/]+/include/" "" _inc "${_rel}")
  # Only headers of pure (host-built) components are checked here.
  string(REGEX MATCH "^[^/]+" _comp "${_rel}")
  if(_comp IN_LIST QZ_HOST_COMPONENTS)
    file(GENERATE OUTPUT "${_src}" CONTENT "#include <${_inc}>\n")
    list(APPEND _qz_header_check_srcs "${_src}")
  endif()
endforeach()
if(_qz_header_check_srcs)
  add_library(qz_header_check OBJECT ${_qz_header_check_srcs})
  target_link_libraries(qz_header_check PRIVATE ${QZ_HOST_COMPONENTS})
  target_compile_features(qz_header_check PRIVATE cxx_std_${QZ_CXX_STANDARD})
  target_compile_options(qz_header_check PRIVATE ${QZ_WARNING_FLAGS} ${QZ_CXX_ONLY_FLAGS})
endif()

# Simulator and host-only tools (added by the UI/simulator work package).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/sim/CMakeLists.txt)
  add_subdirectory(sim)
endif()
