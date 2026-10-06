// Unity entry point: the pytest-embedded menu runner picks cases by name.
#include "unity.h"
#include "unity_test_runner.h"

extern "C" void app_main() {
    unity_run_menu();
}
