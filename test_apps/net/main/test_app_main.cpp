// Unity entry point: the pytest-embedded menu runner picks cases by name.
#include "nvs_flash.h"
#include "unity.h"
#include "unity_test_runner.h"

extern "C" void app_main() {
    // PHY calibration data lives in NVS (CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    unity_run_menu();
}
