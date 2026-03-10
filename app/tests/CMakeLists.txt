# SPDX-License-Identifier: Apache-2.0
# Xi640 DCD Unit Tests — native_posix/native_sim
# Ausfuehren: west build -b native_posix app/app/tests -d build/test
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_dcd_tests)

target_sources(app PRIVATE test_xi640_dcd.c)

# xi640_dcd.h liegt in app/app/inc — relativer Pfad zum Test-Verzeichnis
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../inc
)
