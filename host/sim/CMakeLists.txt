# Host simulator (WP-23, face-only mode): qz_sim_lib (parsing + rendering), the qz_sim executable
# and its tests. Added by host/CMakeLists.txt when this file exists.
add_library(qz_sim_lib STATIC ${CMAKE_CURRENT_LIST_DIR}/sim.cpp)
target_include_directories(qz_sim_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(qz_sim_lib PUBLIC qz_core qz_gfx qz_model qz_time qz_ui qz_faces qz_weather)
target_compile_features(qz_sim_lib PRIVATE cxx_std_${QZ_CXX_STANDARD})
target_compile_options(qz_sim_lib PRIVATE ${QZ_WARNING_FLAGS} ${QZ_CXX_ONLY_FLAGS})

add_executable(qz_sim ${CMAKE_CURRENT_LIST_DIR}/main.cpp)
target_link_libraries(qz_sim PRIVATE qz_sim_lib)
target_compile_features(qz_sim PRIVATE cxx_std_${QZ_CXX_STANDARD})
target_compile_options(qz_sim PRIVATE ${QZ_WARNING_FLAGS} ${QZ_CXX_ONLY_FLAGS})

add_executable(qz_sim_test ${CMAKE_CURRENT_LIST_DIR}/test/sim_test.cpp)
target_link_libraries(qz_sim_test PRIVATE qz_sim_lib GTest::gtest_main)
target_compile_features(qz_sim_test PRIVATE cxx_std_${QZ_CXX_STANDARD})
target_compile_options(qz_sim_test PRIVATE ${QZ_WARNING_FLAGS})
gtest_discover_tests(qz_sim_test DISCOVERY_MODE PRE_TEST)

# Binary-level checks: usage error -> exit code 2; a valid run writes a PNG and exits 0.
add_test(NAME qz_sim_cli_unknown_arg_exit2
  COMMAND sh -c "\"$1\" --bogus >/dev/null 2>&1; test $? -eq 2" sh $<TARGET_FILE:qz_sim>)
add_test(NAME qz_sim_cli_writes_png
  COMMAND $<TARGET_FILE:qz_sim> --face minimal --scale 2 --out ${CMAKE_CURRENT_BINARY_DIR}/cli_face.png)
