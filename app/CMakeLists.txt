# ==============================================================
# Xi640 Project 22 — CMakeLists.txt
# STM32N6 Hybrid USBX/Zephyr UVC ISO
# ==============================================================
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(xi640_usbx_uvc)

# --- Application Sources ---
target_sources(app PRIVATE
    src/main.c
)

# --- USBX Zephyr Port (Phase 3 — noch nicht aktiv) ---
# if(CONFIG_USBX_ZEPHYR_PORT)
#     add_subdirectory(../modules/usbx_zephyr_port usbx_zephyr_port)
# endif()