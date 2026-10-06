include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  SRC_DIRS src third_party/bosch
  PRIV_INCLUDE_DIRS src third_party/bosch
  REQUIRES qz_core qz_hal
)
