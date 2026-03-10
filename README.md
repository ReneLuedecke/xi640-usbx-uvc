# Xi640 — STM32N6 USBX/Zephyr Hybrid UVC ISO

Firmware für die Xi640 Thermalkamera basierend auf STM32N6570-DK.

## Architektur

- **Zephyr RTOS**: DCMIPP, PSRAM, Ethernet/RTSP, VENC H.264, System
- **USBX (Eclipse ThreadX 6.4.1, MIT)**: USB OTG HS, UVC ISO, DMA

## Ziel

- 640×480 YUYV @ 30 FPS über USB HS ISO (~18 MB/s)
- Parallel: Ethernet RTSP RAW (bereits 54.5 MB/s erreicht)

## Voraussetzungen

- West (`pip install west`)
- Zephyr SDK oder ARM GNU Toolchain 14.3
- STM32CubeProgrammer

## Setup

```bash
west init -l app
west update
west build -b stm32n6570_dk/stm32n657xx app
```

## Phasen

| Phase | Ziel | Status |
|-------|------|--------|
| 0 | Architektur, Memory-Regeln | ✅ |
| 1 | Zephyr USB deaktiviert | ✅ |
| 2 | USBX west-Module | ✅ |
| 3 | Zephyr Port-Layer | ✅ |
| 4 | DCD + HS PHY + DMA | 🔄 |
| 5 | UVC ISO Dummy-Stream | ⏳ |
| 6 | DCMIPP Pipeline | ⏳ |
| 7 | USB + Ethernet parallel | ⏳ |
| 8 | Produktisierung | ⏳ |

## Lizenz

Zephyr: Apache-2.0
USBX (Eclipse ThreadX): MIT
Projektcode: Proprietary — Optris GmbH
