idf_component_register(SRCS "test_app_main.cpp" "test_net.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES unity qz_net qz_hal qz_core esp_wifi nvs_flash
                       WHOLE_ARCHIVE)
