# qz_golden: golden-image tool (WP-19). Added to the host build by components/qz_selftest/CMakeLists.txt
# (host mode only). Update: build/<dir>/golden/qz_golden --update ; verify: --check (also a ctest).
add_executable(qz_golden ${CMAKE_CURRENT_LIST_DIR}/main.cpp)
target_link_libraries(qz_golden PRIVATE qz_selftest qz_faces qz_ui qz_gfx qz_core)
target_include_directories(qz_golden PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(qz_golden PRIVATE cxx_std_${QZ_CXX_STANDARD})
target_compile_options(qz_golden PRIVATE ${QZ_WARNING_FLAGS})
target_compile_definitions(qz_golden PRIVATE
  QZ_GOLDEN_DIR="${QZ_ROOT}/components/qz_selftest/golden"
  QZ_GOLDEN_CRC_INC="${QZ_ROOT}/components/qz_selftest/src/golden_crc.inc")
add_test(NAME qz_golden_check
  COMMAND qz_golden --check --out-dir ${CMAKE_CURRENT_BINARY_DIR}/failures)
