# sdkconfig specification

Every symbol below was checked to exist in the pinned tree (`.toolchain/esp-idf`, v6.1); the
"Defined in" column is the Kconfig file. The lead/WP-28 writes `sdkconfig.defaults` (common),
`sdkconfig.defaults.offline` (adds `CONFIG_QZ_RADIO=n`) and keeps `sdkconfig` itself untracked.

## Contents
1. [Common defaults](#1-common-defaults)
2. [Radio build only](#2-radio-build-only)
3. [Quartz Kconfig menu](#3-quartz-kconfig-menu)
4. [Deliberately not set](#4-deliberately-not-set)

## 1. Common defaults

| Key | Value | Why | Defined in |
|---|---|---|---|
| `CONFIG_IDF_TARGET` | `"esp32s3"` | ESP32-S3FN8 | — |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | y | 8 MB in-package flash [R1 s10] | esptool_py/Kconfig.projbuild |
| `CONFIG_ESPTOOLPY_FLASHMODE_QIO` | y | faster image load on every wake | esptool_py/Kconfig.projbuild |
| `CONFIG_ESPTOOLPY_FLASHFREQ_80M` | y | 80 MHz max flash clock [R1 s10] | spi_flash/<chip>/Kconfig.flash_freq |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | see PARTITIONS.md | partition_table/Kconfig.projbuild |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `"partitions.csv"` | | partition_table/Kconfig.projbuild |
| `CONFIG_PARTITION_TABLE_OFFSET` | `0x10000` | 64 KiB bootloader headroom (future secure boot) | partition_table/Kconfig.projbuild |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | y | rollback-capable for future OTA; app marks itself valid after its first good wake | bootloader/Kconfig.app_rollback |
| `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` | y | saves image hashing on ~1440 wakes/day; allowed without secure boot | bootloader/Kconfig.projbuild |
| `CONFIG_BOOTLOADER_LOG_LEVEL_NONE` | y | boot time on every wake | bootloader/Kconfig.log |
| `CONFIG_RTC_CLK_SRC_EXT_CRYS` | y | 32.768 kHz crystal timekeeping (SPEC) | esp_hw_support/port/esp32s3/Kconfig.rtc |
| `CONFIG_RTC_CLK_CAL_CYCLES` | 3000 (default for crystal) | calibration precision vs boot time [TUNE in B8] | esp_hw_support/port/esp32s3/Kconfig.rtc |
| `CONFIG_ESP_SYSTEM_RTC_EXT_XTAL_BOOTSTRAP_CYCLES` | default | crystal start-up kick; revisit if B8 shows start failures (load-cap conflict [R1 s10]) | esp_system/Kconfig |
| `CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT` | y (default) | libc time from RTC + high-res timer | esp_libc/Kconfig |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80` | y | lowest active current; ample for this workload | esp_system/port/soc/esp32s3/Kconfig.cpu |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | y | console on native USB [R1 s11] | esp_stdio/Kconfig |
| `CONFIG_ESP_CONSOLE_SECONDARY_NONE` | y | no UART mirror (UART0 only on test pads) | esp_stdio/Kconfig |
| `CONFIG_LOG_DEFAULT_LEVEL_WARN` | y | battery builds stay quiet; raised at runtime when tethered | log/Kconfig.level |
| `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` | y | allows `log level debug` while tethered | log/Kconfig.level |
| `CONFIG_COMPILER_OPTIMIZATION_SIZE` | y | flash + wake load time; revisit with B9 data | Kconfig |
| `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE` | y | keep IDF asserts in release | Kconfig |
| `CONFIG_COMPILER_CXX_EXCEPTIONS` | n (default) | no exceptions (ARCH section 2) | Kconfig |
| `CONFIG_COMPILER_CXX_RTTI` | n (default) | no RTTI | Kconfig |
| `CONFIG_COMPILER_STACK_CHECK_MODE_NORM` | y | stack smashing detection | Kconfig |
| `CONFIG_ESP_SYSTEM_HW_STACK_GUARD` | y (default) | stack overflow detection | esp_system/Kconfig |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 8192 | app task (ARCH section 20) | esp_system/Kconfig |
| `CONFIG_FREERTOS_HZ` | 1000 | 1 ms tick granularity for gesture timing | freertos/Kconfig |
| `CONFIG_ESP_TASK_WDT_EN` | y; `CONFIG_ESP_TASK_WDT_TIMEOUT_S`=10 | hung wake -> reset -> wake log | esp_system/Kconfig |
| `CONFIG_ESP_INT_WDT` | y (default) | | esp_system/Kconfig |
| `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` | y | never hang on battery | esp_system/Kconfig |
| `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` | y | post-mortem via `idf.py coredump-info`; partition in PARTITIONS.md | espcoredump/Kconfig |
| `CONFIG_ESP_BROWNOUT_DET` | y (default) | brownout = cold boot, time invalid | esp_hw_support/power_supply/port/esp32s3/Kconfig.power |
| `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS` | n | buttons have external pull-ups [R1 s6]; IDF must not add pulls on wake pins | esp_hw_support/Kconfig |
| `CONFIG_ULP_COPROC_ENABLED` | n (default) | RTC slow memory reserved for RtcState + FrameShadow | ulp/Kconfig |
| `CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM` | n (default) | keep RTC data in slow memory; WP-24 may flip it if the map shows slow memory is short (8 KiB each) | esp_system/port/soc/esp32s3/Kconfig.memory |
| `CONFIG_NVS_ENCRYPTION` | n | owner decision Q-04 (ARCH section 13) | nvs_flash/Kconfig |

## 2. Radio build only

Set in `sdkconfig.defaults` (ignored when `CONFIG_QZ_RADIO=n` because nothing references the stack).

| Key | Value | Why | Defined in |
|---|---|---|---|
| `CONFIG_ESP_WIFI_NVS_ENABLED` | n | credentials live in `qz_cred`; no extra NVS writes by the driver | esp_wifi/Kconfig |
| `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE` | y (default) | PHY calibration stored in NVS, faster reconnects; first write once | esp_phy/Kconfig |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 4 | RAM; single short HTTPS session | esp_wifi/Kconfig |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 16 | RAM | esp_wifi/Kconfig |
| `CONFIG_LWIP_SNTP_MAX_SERVERS` | 2 | `pool.ntp.org`, `time.google.com` [ASSUMED choice] | lwip/Kconfig |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` | y | HTTPS validation (SPEC) | mbedtls/Kconfig |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` | y | common CAs only (flash) | mbedtls/Kconfig |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | y | lower TLS heap peak | mbedtls/Kconfig |
| `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS` | y (default) | | esp_http_client/Kconfig |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 | provisioning form browsers send large headers | esp_http_server/Kconfig |

## 3. Quartz Kconfig menu

`main/Kconfig.projbuild`, menu "Quartz". Pure code never reads these; `main/` copies them into
`app::BuildFeatures`.

| Symbol | Type / default | Effect |
|---|---|---|
| `CONFIG_QZ_RADIO` | bool, y | n = offline build: `qz_net` compiles stubs, no Wi-Fi/lwIP/mbedTLS symbols linked (CI checks with `nm`) |
| `CONFIG_QZ_USB_WAKE` | bool, y | EXT0 wake on USB detect; n = USB noticed at the next tick (ARCH section 5) |
| `CONFIG_QZ_SELFTEST_INTERACTIVE` | bool, y | include button/vibration prompts in the self-test |
| `CONFIG_QZ_SNTP_SERVER_1/2` | string, "pool.ntp.org" / "time.google.com" | SNTP servers |

## 4. Deliberately not set

- `CONFIG_PM_ENABLE` (automatic light sleep): the app sleeps explicitly; DFS adds wake jitter.
- `CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`: irreversible eFuse changes; owner decision (Q-04).
- `CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS`: power-on validation stays as a corruption guard.
- `CONFIG_FREERTOS_UNICORE`: keep IDF defaults; revisit only if B9 shows a measurable gain.
