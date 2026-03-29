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

# DCD-Dateien explizit — ux_dcd_stm32_callback.c kommt aus lokaler Kopie
# (app/usbx_zephyr_port/src/) damit wir HAL_PCD_ResetCallback patchen koennen
# ohne das USBX-Modul zu modifizieren.
set(USBX_DCD_SRC
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_endpoint_create.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_endpoint_destroy.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_endpoint_reset.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_endpoint_stall.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_endpoint_status.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_frame_number_get.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_function.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_initialize.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_initialize_complete.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_interrupt_handler.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_transfer_abort.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_transfer_request.c
    # transfer_run.c: lokale Kopie mit __DSB()/__ISB() vor HAL_PCD_EP_Transmit/Receive
    # (ARM Memory Ordering Fix — DMA liest Buffer vor CPU-Write-Abschluss ohne Barrier)
    ${PORT_SRC}/ux_dcd_stm32_transfer_run.c
    ${USBX_ROOT}/common/usbx_stm32_device_controllers/ux_dcd_stm32_uninitialize.c
    ${PORT_SRC}/ux_dcd_stm32_callback.c
)
target_sources(app PRIVATE ${USBX_DCD_SRC})

file(GLOB USBX_UVC_SRC
    ${USBX_ROOT}/common/usbx_device_classes/src/ux_device_class_video_*.c
)
target_sources(app PRIVATE ${USBX_UVC_SRC})

file(GLOB USBX_CDC_ACM_SRC
    ${USBX_ROOT}/common/usbx_device_classes/src/ux_device_class_cdc_acm_*.c
)
target_sources(app PRIVATE ${USBX_CDC_ACM_SRC})

# STM32N6 HAL PCD Sources — nicht automatisch von Zephyr eingebunden
# da CONFIG_USB_DEVICE_STACK=n. Manuell einbinden fuer USBX DCD.
set(HAL_PCD_ROOT ${WORKSPACE_ROOT}/modules/hal/stm32/stm32cube/stm32n6xx/drivers)
target_sources(app PRIVATE
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pcd.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pcd_ex.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_ll_usb.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pwr.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_pwr_ex.c
    ${HAL_PCD_ROOT}/src/stm32n6xx_hal_rif.c
)

target_compile_definitions(app PRIVATE
    UX_STANDALONE
    UX_DEVICE_SIDE_ONLY
    UX_DCD_STM32_MAX_ED=9
    UX_DEVICE_BIDIRECTIONAL_ENDPOINT_SUPPORT
    HAL_PCD_MODULE_ENABLED
    HAL_PWR_MODULE_ENABLED
    HAL_RIF_MODULE_ENABLED
    # ST-Referenz-Angleichung (x-cube-n6-camera-capture, ux_user.h)
    UX_DEVICE_ENDPOINT_BUFFER_OWNER=1   # Klasse verwaltet EP-Buffer (ST: 1, default: 0)
    UX_ENABLE_ASSERT                    # USBX-interne Assertions aktivieren
)
