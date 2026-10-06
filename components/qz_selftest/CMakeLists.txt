include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  REQUIRES qz_core qz_hal qz_time qz_model qz_gfx qz_ui qz_faces qz_settings qz_ssd1681 qz_bma423 qz_power
)

# Host build only: the golden tool (host/golden) is built next to the pure components.
if(NOT COMMAND idf_component_register AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../host/golden/CMakeLists.txt)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../host/golden ${CMAKE_BINARY_DIR}/golden)
endif()
