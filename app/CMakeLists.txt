# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_usbx_uvc)

target_sources(app PRIVATE src/main.c)

add_subdirectory(usbx_zephyr_port)
