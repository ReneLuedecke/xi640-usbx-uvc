# Phase 1: Zephyr UVC Fork + Basis verifizieren

## Kontext

Wir wechseln von USBX auf Zephyr's nativen USB Stack für UVC. 
Projekt 21 (D:\Zephyr_Workbench\project_22\project_21\) hat funktionierenden Bulk UVC Code.
Ziel: ISO High-Bandwidth UVC auf dem STM32N6570-DK.

## Aufgabe

### 1. USBX-Code deaktivieren

In `app/CMakeLists.txt`: 
- Entferne ALLE USBX-bezogenen Sources und Includes (usbx_zephyr_port, st_usbx Module)
- Entferne ALLE USBX Compile-Definitionen (UX_STANDALONE, UX_DEVICE_SIDE_ONLY, etc.)
- Entferne xi640_dcd.c, xi640_uvc_stream.c, xi640_uvc_descriptors.c, xi640_st_test.c, usbx_zephyr.c aus dem Build
- Behalte die HAL RIF Source (stm32n6xx_hal_rif.c) — die brauchen wir eventuell

### 2. Projekt 21 Code übernehmen

Kopiere aus `D:\Zephyr_Workbench\project_22\project_21\`:
- `src/main.c` → `app/src/main.c` (ersetzt den alten main.c)
- `app.overlay` → `app/app.overlay` (ersetzt/merged mit bestehendem)
- `prj.conf` → `app/prj.conf` (ersetzt den alten — WICHTIG: enthält device_next Config)
- `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` bleibt (AXISRAM etc.)

### 3. CMakeLists.txt vereinfachen

```cmake
cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_uvc_iso)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE src/main.c)
```

### 4. Board Overlay mergen

Der bestehende `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` hat AXISRAM-Aktivierung.
Stelle sicher dass er mit dem Projekt 21 Overlay kompatibel ist. Projekt 21 braucht:
- `zephyr,console = &usart1`
- `zephyr,shell-uart = &usart1`

Und unser Overlay braucht:
- `&axisram3/4/5/6 { status = "okay"; }` (für spätere Frame-Buffer)
- PSRAM Config (für später)

Merge beide — AXISRAM + Console.

### 5. app.overlay

```dts
/ {
    chosen {
        zephyr,camera = &sw_generator;
    };

    sw_generator: sw-generator {
        compatible = "zephyr,video-sw-generator";
        status = "okay";
    };

    uvc: uvc {
        compatible = "zephyr,uvc-device";
        status = "okay";
    };
};

&aps256xxn_obr {
    max-frequency = <DT_FREQ_M(100)>;
    read-latency = <4>;
    write-latency = <1>;
    fixed-latency;
    burst-length = <3>;
    rbx;
    st,csbound = <11>;
};
```

### 6. prj.conf

Übernimm Projekt 21 prj.conf, passe an:
```conf
# Project 22 Phase 6: UVC ISO High-Bandwidth
CONFIG_LOG=y
CONFIG_POLL=y
CONFIG_HWINFO=y

# USB device_next
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_UDC_BUF_POOL_SIZE=32768
CONFIG_UDC_DRIVER_LOG_LEVEL_WRN=y

# USBD Video
CONFIG_USBD_VIDEO_CLASS=y
CONFIG_USBD_LOG_LEVEL_WRN=y
CONFIG_USBD_VIDEO_LOG_LEVEL_WRN=y

# Video
CONFIG_VIDEO=y
CONFIG_VIDEO_SW_GENERATOR=y
CONFIG_VIDEO_BUFFER_POOL_NUM_MAX=4
CONFIG_VIDEO_BUFFER_POOL_SZ_MAX=614400
CONFIG_VIDEO_LOG_LEVEL_WRN=y

CONFIG_USBD_VIDEO_NUM_BUFS=8
CONFIG_USBD_VIDEO_MAX_FORMATS=8
CONFIG_USBD_VIDEO_MAX_FRMIVAL=4
CONFIG_UDC_STM32_MAX_QMESSAGES=64

# Stack
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_UDC_STM32_STACK_SIZE=2048

# USB descriptor
CONFIG_SAMPLE_USBD_PID=0x0022
CONFIG_SAMPLE_USBD_PRODUCT="Xi640 UVC ISO"
CONFIG_SAMPLE_USBD_MANUFACTURER="Optris"
```

### 7. Build + Test

```bash
west build -b stm32n6570_dk/stm32n657xx/fsbl -d build/primary --pristine app
west flash -d build/primary
```

### Erwartetes Ergebnis

- Windows Gerätemanager: "Xi640 UVC ISO" als USB-Videogerät
- AMCap/VLC zeigt Testbild (Colorbar)
- Bulk Transfer, ~1.5 FPS (wie Projekt 21)
- Serial Log: "Streaming started!" + Benchmark-Ausgabe

### Erfolgskriterium

Exakt gleiches Verhalten wie Projekt 21. Erst wenn das bestätigt ist → weiter zu Phase 2.

## CLAUDE.md Update

Aktualisiere CLAUDE.md mit:
- Phase 6: UVC ISO High-Bandwidth (Zephyr device_next)
- Architektur: Zephyr USB Stack statt USBX
- Plan: 6 Phasen, siehe option1_iso_uvc_plan.md
