# SPDX-License-Identifier: Apache-2.0
# Xi640 USBX Zephyr Port Layer
# KEIN cmake_minimum_required — wird via add_subdirectory() eingebunden

set(WORKSPACE_ROOT ${ZEPHYR_BASE}/..)
set(USBX_ROOT  ${WORKSPACE_ROOT}/modules/st_usbx)
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

# STM32N6 HAL PCD Sources — nicht automatisch von Zephyr eingebunden
# da CONFIG_USB_DEVICE_STACK=n. Manuell einbinden fuer USBX DCD.
set(HAL_PCD_ROOT ${WORKSPACE_ROOT}/modules/hal/stm32/stm32cube/stm32n6xx/drivers)
target_sources(app PRIVATE
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pcd.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pcd_ex.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_ll_usb.c
)

target_compile_definitions(app PRIVATE
    UX_STANDALONE
    UX_DEVICE_SIDE_ONLY
    UX_DCD_STM32_MAX_ED=9
    UX_DEVICE_BIDIRECTIONAL_ENDPOINT_SUPPORT
    HAL_PCD_MODULE_ENABLED
)
