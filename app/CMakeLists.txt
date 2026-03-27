cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_uvc_dcmipp)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE
    src/main.c
    src/lynred_atto640d.c
    src/atto640d_video_stub.c
    src/dcmipp_capture.c
)

# DCMIPP Frame-Buffer in AXISRAM1 @ 0x34000000 (vor Zephyr Heap)
zephyr_linker_sources(DATA_SECTIONS sections/dcmipp_buf.ld)
