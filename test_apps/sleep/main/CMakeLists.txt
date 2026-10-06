idf_component_register(SRCS "test_app_main.cpp" "test_sleep.cpp" "test_buses.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES unity qz_platform qz_hal qz_core qz_board esp_driver_gpio esp_driver_i2c
                             esp_driver_spi esp_adc nvs_flash esp_hw_support
                       PRIV_INCLUDE_DIRS "../../../components/qz_platform/src"
                       WHOLE_ARCHIVE)
