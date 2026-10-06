# Owner-run Unity tests for qz_platform (WP-24). Build and flash from this directory:
# Build (from this directory, inside the pinned IDF env: tools/idf.sh bash):
#   idf.py -B ../../build/platform-test -DSDKCONFIG=../../build/platform-test/sdkconfig build
#   idf.py -B ../../build/platform-test -p PORT flash monitor   # or: pytest --target esp32s3
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
# Trim the build: main plus whatever it depends on.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qz_platform_test)
