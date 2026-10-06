idf_component_register(
    SRCS "main.cpp"
    REQUIRES qz_app qz_platform qz_net
    PRIV_REQUIRES app_update esp_system esp_hw_support log freertos)
