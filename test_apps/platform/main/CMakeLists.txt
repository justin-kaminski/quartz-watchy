idf_component_register(SRCS "test_app_main.cpp" "test_kv_store.cpp" "test_rtc_memory.cpp"
                            "test_system.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES unity qz_platform qz_hal qz_core esp_adc nvs_flash
                       PRIV_INCLUDE_DIRS "../../../components/qz_platform/src"
                       WHOLE_ARCHIVE)
