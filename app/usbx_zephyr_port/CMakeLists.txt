# SPDX-License-Identifier: Apache-2.0
# Xi640 USBX Zephyr Port Layer
# KEIN cmake_minimum_required — wird via add_subdirectory() eingebunden

set(USBX_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/st_usbx)
set(PORT_INC   ${CMAKE_CURRENT_SOURCE_DIR}/inc)
set(PORT_SRC   ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_include_directories(app PRIVATE
    ${PORT_INC}
    ${USBX_ROOT}/common/core/inc
    ${USBX_ROOT}/common/usbx_stm32_device_controllers
    ${USBX_ROOT}/common/usbx_device_classes/inc
)

target_sources(app PRIVATE ${PORT_SRC}/ux_port.c)

file(GLOB USBX_CORE_SRC
    ${USBX_ROOT}/common/core/src/ux_device_stack_*.c
    ${USBX_ROOT}/common/core/src/ux_system_*.c
    ${USBX_ROOT}/common/core/src/ux_utility_*.c
)
target_sources(app PRIVATE ${USBX_CORE_SRC})

file(GLOB USBX_DCD_SRC
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_*.c
)
target_sources(app PRIVATE ${USBX_DCD_SRC})

file(GLOB USBX_UVC_SRC
    ${USBX_ROOT}/common/usbx_device_classes/src/ux_device_class_video_*.c
)
target_sources(app PRIVATE ${USBX_UVC_SRC})

target_compile_definitions(app PRIVATE
    UX_STANDALONE
    UX_DEVICE_SIDE_ONLY
)
