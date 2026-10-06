include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  SRC_DIRS src third_party/bosch
  PRIV_INCLUDE_DIRS src third_party/bosch
  REQUIRES qz_core qz_hal
)

# Vendored Bosch C (third_party/bosch, unmodified, see PROVENANCE.md): suppress warnings for these
# files only, so -Werror from IDF's default C flags never applies to code we must not edit.
file(GLOB _qz_bosch_c CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/third_party/bosch/*.c")
set_source_files_properties(${_qz_bosch_c} PROPERTIES COMPILE_OPTIONS "-w")
