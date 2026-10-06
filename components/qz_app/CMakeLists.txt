include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  REQUIRES qz_core qz_hal qz_board qz_time qz_model qz_gfx qz_ssd1681 qz_bma423 qz_settings qz_steps qz_power qz_weather qz_conn qz_ui qz_faces qz_console qz_selftest
)
