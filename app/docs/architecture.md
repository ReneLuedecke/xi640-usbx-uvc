# Xi640 — System Architecture

## Subsystem-Aufteilung

```
┌─────────────────────────────────────────────────────┐
│                   STM32N6570                        │
│                                                     │
│  ┌──────────────────┐    ┌─────────────────────┐   │
│  │     ZEPHYR       │    │       USBX           │   │
│  │                  │    │                      │   │
│  │  DCMIPP          │    │  USB OTG HS (exkl.)  │   │
│  │  PSRAM           │    │  UVC ISO             │   │
│  │  Ethernet/RTSP   │    │  DMA                 │   │
│  │  VENC H.264      │    │  STM32 DCD           │   │
│  │  System Mgmt     │    │                      │   │
│  └────────┬─────────┘    └──────────┬───────────┘   │
│           │                         │               │
│           └──── USBX Port Layer ────┘               │
│                 (Zephyr Primitiven)                  │
└─────────────────────────────────────────────────────┘
```

## Grundsatzentscheidungen

| Entscheidung | Wert | Begründung |
|---|---|---|
| USB Mode | ISO first | 24.6 MB/s theoretisch vs. 14 MB/s Bulk-Ceiling |
| USBX Quelle | stm32-mw-usbx v6.4.0 | ST DCD bereits integriert |
| ThreadX | eclipse-threadx v6.5.0 | MIT Lizenz, USBX-Basis |
| Zephyr Version | 4.3.99 (main) | aps256xxn_obr Node vorhanden |
| Build Variante | `//fsbl` | Code in AXISRAM → SW-Breakpoints |

## 3 No-Gos

1. **Zephyr USB + USBX gleichzeitig** → Geisterbugs (erlebt: CDC+UVC Konflikt)
2. **ISO Buffer in PSRAM ohne Cache-Konzept** → läuft 10 Sekunden (erlebt: VENC EWL)
3. **Pipeline zu früh koppeln** → 5 Unbekannte gleichzeitig (mehrfach erlebt)

## USB Bandwidth-Rechnung

```
Frame:      640 × 480 × 2 = 614.400 Bytes
30 FPS:     614.400 × 30  = 18,43 MB/s  (Ziel)
USB HS ISO: 1024 × 3 × 8  = 24,58 MB/s (Maximum)
Reserve:    6,15 MB/s (25% Puffer) ✅
```

## Thread-Prioritäten

| Thread | Priorität | Stack | Zweck |
|--------|-----------|-------|-------|
| USBX ISO TX | 2 | 4096 | USB DMA feed |
| DCMIPP DMA | 3 | 2048 | Frame capture |
| VENC H.264 | 5 | 8192 | Encoding |
| Ethernet TX | 6 | 4096 | RTSP streaming |
| Main | 10 | 8192 | System control |
| Zephyr SWQ | 14 | 4096 | System workqueue |

## west Module

| Modul | Remote | SHA | Pfad |
|-------|--------|-----|------|
| stm32-mw-usbx | STMicroelectronics | 280f6bc | modules/st_usbx |
| threadx | eclipse-threadx | 3726d790 | modules/threadx |
