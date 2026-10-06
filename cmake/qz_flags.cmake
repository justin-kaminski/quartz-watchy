# Compiler flags shared by every Quartz-owned target (host and firmware).
# Vendored third-party code must NOT use these (it opts out via qz_component(THIRD_PARTY)).

set(QZ_CXX_STANDARD 23 CACHE STRING "C++ standard used for Quartz code (ADR-0004; fall back to 20 if a compiler rejects a used feature)")

set(QZ_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wcast-align
    -Wformat=2
    -Wimplicit-fallthrough
    -Wdouble-promotion
    -Wundef
    -Werror)

# Firmware-style C++: no exceptions, no RTTI (see docs/ARCHITECTURE.md, coding conventions).
set(QZ_CXX_ONLY_FLAGS -fno-exceptions -fno-rtti)

# IDF-only platform components include IDF/FreeRTOS headers full of C-style-cast macros
# (pdMS_TO_TICKS, portMAX_DELAY, ...), so they use a relaxed subset.
set(QZ_WARNING_FLAGS_PLATFORM
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wformat=2
    -Wimplicit-fallthrough
    -Werror)
