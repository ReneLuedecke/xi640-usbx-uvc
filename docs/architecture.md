# Xi640 — System Architecture
## Project 22: STM32N6 Hybrid USBX/Zephyr UVC ISO

### Subsystem Owner

| Subsystem         | Owner  | Notes                          |
|-------------------|--------|-------------------------------|
| DCMIPP            | Zephyr | DMA, DTS, Thomas               |
| PSRAM APS256XXN   | Zephyr | 100 MHz, 202 MB/s read         |
| Ethernet / RTSP   | Zephyr | 54.5 MB/s erreicht             |
| VENC H.264        | Zephyr | Optionaler Preview-Pfad        |
| **USB OTG HS**    | **USBX** | **DMA, ISO, 30 FPS Ziel**   |

### Pipeline

```
Sensor → DCMIPP → AXI SRAM Frame Ring → Frame Dispatcher
                                          |→ USBX UVC ISO  (18 MB/s RAW)
                                          |→ Ethernet RTSP (Zephyr)
                                          └→ VENC H.264    (optional)
```

### Hot/Cold Memory Regel

- **AXI SRAM (Hot)**: USB ISO Buffer, UVC Header, TX Payload, Frame Ring
- **PSRAM (Cold)**: Große Queues, Archiv, Debug
- **NIEMALS**: ISO-TX-Buffer in PSRAM (XSPI-Latenz zerstört ISO Timing)

### Thread-Prioritäten

| Thread         | Priorität | Bemerkung              |
|----------------|-----------|------------------------|
| USB ISR        | ISR       | SOF darf nicht verhungern |
| DCMIPP ISR     | ISR       | Capture darf nicht stallen |
| USBX Stream    | 2         | Höchste Thread-Prio    |
| Camera Disp.   | 3         |                        |
| Ethernet       | 5         |                        |
| App / Shell    | 10+       |                        |
