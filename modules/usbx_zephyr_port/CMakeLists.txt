# modules/usbx_zephyr_port/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

if(CONFIG_USBX_ZEPHYR_PORT)
    zephyr_library_named(usbx_zephyr_port)

    zephyr_library_sources(
        src/ux_port.c
    )

    zephyr_library_include_directories(
        include
        ${CMAKE_CURRENT_SOURCE_DIR}/../st_usbx/Middlewares/ST/usbx/common/core/inc
    )
endif()
