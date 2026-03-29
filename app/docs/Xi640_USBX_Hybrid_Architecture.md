# STM32N6 USBX Hybrid Architecture

**Xi640 – 640×480 YUYV @ 30 FPS over USB HS (ISO)**

| | |
|---|---|
| Projekt | Optris Xi640 Thermalkamera |
| Board | STM32N6570-DK (Cortex-M55) |
| RTOS | Zephyr 4.3.99 |
| USB Middleware | USBX (stm32-mw-usbx v6.4.0, Standalone) |
| Version | v1.0 — aktualisiert 2026-03-12 |
| Status | Phase 5 in Arbeit |

---

## 1. Executive Summary

Wir verfolgen einen Hybrid-Ansatz: Zephyr als Systemplattform, USBX exklusiv für USB Device UVC ISO.

**Zephyr** übernimmt: PSRAM, Ethernet/RTSP, DCMIPP, VENC (H.264), Logging, Infrastruktur.

**USBX** übernimmt: STM32 OTG HS DMA, High-Bandwidth Isochronous, deterministischer Transferpfad.

**Warum Hybrid?** Der Zephyr UVC Bulk-Stack zeigt strukturell einen Deckel bei ~14–16 Mbit/s. Unser Ziel benötigt ~18,4 MB/s kontinuierlich. ST demonstriert im [x-cube-n6-camera-capture](https://github.com/STMicroelectronics/x-cube-n6-camera-capture) Repository 640×480 YUV422 @ 30 FPS auf identischer Hardware — das validiert DCMIPP Performance, AXI SRAM Bandbreite, DMA-Fähigkeit und Hardware-Bus-Topologie.

Die verbleibende Herausforderung ist die korrekte High-Bandwidth ISO Konfiguration.

---

## 2. Architektur

```
Sensor → DCMIPP → AXI SRAM Ring Buffer → Frame Dispatcher ─┬→ USBX UVC ISO (USB HS)
                                                             └→ Ethernet RTSP (Zephyr)
                                                             └→ VENC H.264 (optional)
```

### Warum nicht Zephyr Bulk UVC?

Gemessener Durchsatz: ~14–16 Mbit/s bei 640×480 YUYV (2–3 FPS). Ursachen: Callback-/Workqueue-Overhead, kein DMA im UDC, 1 Transfer pro Packet, FIFO-Limitierung, kein ISO Support. Bulk skaliert hier strukturell nicht.

---

## 3. USBX Standalone-Modus

Seit Eclipse ThreadX 6.4.1 kann USBX RTOS-unabhängig laufen. Erforderliche Mappings:

| ThreadX API | Zephyr Mapping |
|---|---|
| `tx_thread_create` | `k_thread_create` |
| `tx_thread_suspend/resume` | `k_thread_suspend/resume` |
| `tx_semaphore_get/put` | `k_sem_take/give` |
| `tx_mutex_get/put` | `k_mutex_lock/unlock` |
| `tx_time_get` | `k_uptime_get_32` |
| `tx_byte_pool` | statischer Heap |

### Kritische Standalone-Anforderungen (aus Phase 5 Debugging)

| Anforderung | Detail |
|---|---|
| `_ux_utility_interrupt_disable()` | MUSS als echte C-Funktion implementiert werden — `__get_PRIMASK()` + `__disable_irq()`. Makros reichen NICHT. |
| `_ux_utility_interrupt_restore()` | MUSS als echte C-Funktion implementiert werden — `__enable_irq()` wenn flags==0. |
| `ux_system_tasks_run()` | Muss in dediziertem Thread (Prio 2) kontinuierlich gepumpt werden. `k_sleep(K_MSEC(1))` zwischen Aufrufen. |
| Thread-Starvation | `k_yield()` in Idle-Loops verhungert niedrigere Threads. IMMER `k_sleep(K_MSEC(1))` verwenden. |

---

## 4. Bandbreite

```
Frame:      640 × 480 × 2 = 614.400 Bytes
30 FPS:     614.400 × 30  = 18,43 MB/s  (Ziel)
USB HS ISO: 1024 × 3 × 8  = 24,58 MB/s  (Maximum)
Reserve:    6,15 MB/s (25% Puffer) ✅
```

---

## 5. UVC ISO Konfiguration

### Endpoint Descriptor (High Bandwidth)

```
bEndpointAddress = 0x81 (IN)
bmAttributes     = ISO + Asynchron
wMaxPacketSize   = 0x1400 (1024 × 3 Transactions)
bInterval        = 1
```

### Probe/Commit

```
frameInterval           = 333333 (30 FPS, 100ns-Einheiten)
maxVideoFrameSize       = 614400
maxPayloadTransferSize  = 24576
```

---

## 6. STM32N6 OTG HS Hardware Setup

### PHY Initialisierung (aus ST Referenzprojekt — kritische Reihenfolge)

```c
// 1. VDD USB Supply aktivieren (PFLICHT für HS!)
__HAL_RCC_PWR_CLK_ENABLE();
HAL_PWREx_EnableVddUSBVMEN();
while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);  // Timeout hinzufügen!
HAL_PWREx_EnableVddUSB();

// 2. OTG HS Peripheral Clock
__HAL_RCC_USB1_OTG_HS_CLK_ENABLE();

// 3. PHY Reference Clock (24 MHz HSE auf DK-Board)
USB1_HS_PHYC->USBPHYC_CR &= ~(0x7U << 0x4U);
USB1_HS_PHYC->USBPHYC_CR |= (0x2U << 0x4U);   // FSEL = 0x2

// 4. PHY Clock enablen
__HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();
```

### FIFO Konfiguration

```
Gesamt: 4096 Words (16384 Bytes)
RX FIFO:         1024 Words (4096 Bytes)
EP0 TX FIFO:      512 Words (2048 Bytes)
EP1 ISO TX FIFO: 2048 Words (8192 Bytes)
Belegt: 3584/4096 Words (87,5%)
```

### DMA

```
GAHBCFG: DMAEN = 1, HBSTLEN = INCR4 (0x3, HAL Default)
```

INCR16 nur wenn nötig — ISO-Buffer liegen in AXISRAM, nicht PSRAM.

### OTG Core ID

STM32N6570-DK: `0x4F54411A` (nicht 310A wie ursprünglich erwartet).

---

## 7. Memory-Strategie

### AXI SRAM — Hot Path (kein Cache-Problem)

| Region | Adresse | Größe | Verwendung |
|---|---|---|---|
| AXISRAM2 | 0x34180400 | 511 KB | FSBL Code + BSS |
| AXISRAM3 | 0x34200000 | 512 KB | Frame Buffer (614 KB) + USBX Pool (128 KB) |
| AXISRAM4 | 0x34280000 | 512 KB | Frame Buffer Pong |
| AXISRAM5 | 0x34300000 | 512 KB | VENC Input Buffer |
| AXISRAM6 | 0x34380000 | 512 KB | VENC Output / Netzwerk |

### PSRAM — Cold Path (Cache-Konzept erforderlich)

Netzwerk-Buffer, JPEG/H.264 Output, Logging, große Lookup-Tables. 100 MHz, ~109 MB/s Write, ~202 MB/s Read.

### Kritische Regeln

- ISO Buffer NIEMALS in PSRAM — XSPI-Latenz zerstört ISO Timing.
- Alle DMA-Buffer 32-Byte aligned (`__attribute__((aligned(32)))`).
- Cache-Flush VOR jedem DMA TX, Cache-Invalidate NACH jedem DMA RX.
- AXISRAM-Bänke müssen über originale DTS-Knoten (`&axisram3 { status = "okay"; }`) aktiviert werden — Custom `memory@xxxxx` Knoten werden vom RAMCFG-Treiber NICHT erkannt.
- Große Buffer per fester Adresse platzieren (`static uint8_t * const buf = (uint8_t *)0x34200000`), nicht per `section()`.

---

## 8. Thread-Modell

| Thread / ISR | Priorität | Stack | Zweck |
|---|---|---|---|
| USB ISR (OTG HS) | ISR | — | SOF-Interrupt |
| DCMIPP ISR | ISR | — | Frame-Capture |
| USBX Tasks Pumpe | 2 | 2048 | `ux_system_tasks_run()` |
| UVC Streaming | 3 | 4096 | ISO Payload senden |
| DCMIPP Dispatcher | 4 | 2048 | Frame-Verteilung |
| VENC H.264 | 5 | 8192 | Encoding |
| Ethernet TX | 6 | 4096 | RTSP Streaming |
| Main / App | 10 | 16384 | System Control |
| Zephyr SWQ | 14 | 4096 | System Workqueue |

**Regeln:** Kein Logging im Hot Path. Kein malloc im Streaming. Keine Mutex in ISR. `k_yield()` nur für kurze Retries — `k_sleep(K_MSEC(1))` in Idle-Loops.

---

## 9. Bus Contention

Gleichzeitig aktive DMA-Master: DCMIPP, USB, Ethernet, CPU.

Mitigation: AXI SRAM für In-Flight-Buffer, Stresstest USB + Ethernet, QoS-Tuning falls nötig. ISO-Buffer in AXISRAM eliminiert PSRAM/XSPI als Engpass.

---

## 10. Phasenplan

| Phase | Titel | Status |
|---|---|---|
| 0 | Architektur fixieren | ✅ Done |
| 1 | Zephyr USB deaktivieren + Heartbeat | ✅ Done |
| 2 | USBX west-Module einbinden | ✅ Done |
| 3 | USBX Zephyr Port-Layer | ✅ Done |
| 4 | DCD + HS PHY + DMA + Cache | 🔄 90% |
| 5 | UVC ISO Dummy-Stream | 🔄 In Arbeit |
| 6 | DCMIPP RAW-Pipeline | ⏳ Offen |
| 7 | Parallelbetrieb USB + Ethernet | ⏳ Offen |
| 8 | Produktisierung | ⏳ Offen |

---

## 11. Failure Modes

| Problem | Ursache | Status |
|---|---|---|
| 10 FPS Limit | Single Transaction statt 3× High-Bandwidth | Deskriptor konfiguriert |
| Stream stoppt nach Sekunden | Cache/DMA Race | Cache-Wrapper implementiert |
| FPS Drop bei Ethernet | AXI Contention | Noch nicht getestet |
| Nur jeder zweite Frame korrekt | Frame-ID Bit falsch | FID-Toggle implementiert |
| Nur 12 Mbit/s (Full-Speed) | VDD USB nicht aktiviert + PHY Clock fehlt | Fix identifiziert |
| Hard Fault bei Enumeration | IRQ enabled vor USBX Stack Init | Gefixt |
| Thread-Starvation (Log bricht ab) | `k_yield()` in Idle-Loop | Gefixt (`k_sleep`) |
| USBX Enumeration Race Condition | `_ux_utility_interrupt_disable/restore()` fehlt | Fix identifiziert |
| Bus Fault beim Boot | AXISRAM-Bänke nicht per RAMCFG aktiviert | Gefixt (DTS Overlay) |

---

## 12. Testmatrix

### Windows

- USBTreeView → 480M High-Speed verifizieren
- OBS / AMCap → 30 FPS verifizieren
- 10 Minuten Dauerlauf
- 10× Reconnect ohne Absturz

### Linux

- `v4l2-ctl` — Format + FPS
- `usbmon` + Wireshark — Protokoll-Level
- `dmesg` — Fehleranalyse

---

## 13. Referenzen

- ST Referenzprojekt (funktionierendes USBX UVC auf STM32N6570-DK): https://github.com/STMicroelectronics/x-cube-n6-camera-capture
- USBX stm32-mw-usbx v6.4.0: SHA `280f6bc`
- Eclipse ThreadX v6.5.0: SHA `3726d790`
