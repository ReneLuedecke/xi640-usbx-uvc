cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay"
)

# Diagnose-Modus: SW-Generator Colorbar → UVC (DCMIPP läuft, liefert aber nicht an USB).
# Build: west build ... -- -DXI640_DIAG_SWGEN=ON
# Vorteil: Kein '.' im CMake-Argument-Wert → kein PowerShell-Parsing-Problem.
option(XI640_DIAG_SWGEN "SW-Generator Colorbar Diagnose-Modus (statt DCMIPP)" OFF)
if(XI640_DIAG_SWGEN)
  set(EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/diag_swgen.conf")
  message(STATUS "XI640: Diagnose-Modus aktiv — EXTRA_CONF_FILE=${EXTRA_CONF_FILE}")
endif()

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
