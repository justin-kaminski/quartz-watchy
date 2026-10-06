include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  REQUIRES qz_core qz_hal qz_board qz_console
  PRIV_REQUIRES esp_driver_spi esp_driver_i2c esp_driver_gpio esp_adc nvs_flash esp_hw_support esp_system esp_timer console esp_driver_usb_serial_jtag esp_app_format esp_rom freertos log
  PLATFORM
)

# Short git hash (root CMakeLists.txt) for IdfSystem::firmware(); only this component sees it, so
# a new hash recompiles one file instead of the whole firmware.
if(COMMAND idf_component_register AND QZ_GIT_HASH)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE "QZ_GIT_HASH=\"${QZ_GIT_HASH}\"")
endif()
