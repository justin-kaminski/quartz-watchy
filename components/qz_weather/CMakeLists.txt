include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  PRIV_REQUIRES cjson
  REQUIRES qz_core qz_hal qz_time qz_model
)
