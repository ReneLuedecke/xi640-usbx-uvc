# CLAUDE.md

Dieses File gibt Claude Code den vollständigen Projektkontext.
Sprache: **Deutsch** (Docs, Kommentare, Commits).

---

## Projektübersicht

Xi640 ist Firmware für eine Optris Thermalkamera auf dem STM32N6570-DK (Cortex-M55).
**Zephyr-native Architektur:** Zephyr RTOS + Zephyr `device_next` USB Stack. USBX wurde aufgegeben (USB HS IN-Transfer xact errors — nicht in vertretbarer Zeit lösbar).

| Eigenschaft | Wert |
|---|---|
| MCU | STM32N6570 (Cortex-M55) |
| Board | STM32N6570-DK |
| RTOS | Zephyr 4.3.99 (main branch) |
| USB Stack | Zephyr `device_next` (CONFIG_USB_DEVICE_STACK_NEXT=y) |
| USB Treiber | Zephyr UDC DWC2 (udc_dwc2.c — ISO bereits für UAC2 vorhanden) |
| Build-Variante | `//fsbl` — Code in AXISRAM → SW-Breakpoints |
| Zielformat | 640×480 YUYV @ 30 FPS = 18,43 MB/s |
| Aktueller USB-Modus | **Bulk** (DCMIPP→UVC läuft — echte Thermaldaten @ ~19-22 FPS ✅) |
| Ziel USB-Modus | **ISO High-Bandwidth** (3×1024 = 24,6 MB/s max — Fork `usbd_uvc_iso.c` bereit) |
| Ethernet | RAW/RTSP parallel (54,5 MB/s bereits erreicht) |

---

## Build-Kommandos

```bash
# Ersteinrichtung (einmal, aus Parent-Verzeichnis)
west init -l app
west update

# Build (FSBL-Variante — nötig für SW-Breakpoints in AXISRAM)
west build -b stm32n6570_dk/stm32n657xx/fsbl app -d build/primary

# Rebuild nach Code-Änderungen
west build -d build/primary

# Rebuild nach DTS/Overlay-Änderungen (--pristine PFLICHT)
west build --pristine -b stm32n6570_dk/stm32n657xx/fsbl -d build/primary app

# Flash
west flash -d build/primary

# Diagnose-Build: SW-Generator Colorbar → UVC (DCMIPP läuft im Hintergrund)
# Verifiziert UVC-Pipeline unabhängig von DCMIPP-Übergabe
west build -b stm32n6570_dk/stm32n657xx/fsbl -d build/primary -p always app \
  -- -DEXTRA_CONF_FILE=diag_swgen.conf
```

⚠️ Externer Flash (0x70xxxxxx) unterstützt keine SW-Breakpoints.
⚠️ `--pristine` Build PFLICHT nach DTS-Overlay-Änderungen — Zephyr cached generierte DTS-Dateien.

---

## Architektur — Zephyr-native (Aktuell: Bulk, Ziel: ISO)

```
┌─────────────────────────────────────────────────────┐
│                   STM32N6570                        │
│                                                     │
│  ┌──────────────────────────────────────────────┐   │
│  │                    ZEPHYR                    │   │
│  │                                              │   │
│  │  Zephyr device_next USB Stack               │   │
│  │  ├── main.c (DCMIPP-Sim, video_enqueue)     │   │
│  │  ├── CONFIG_USBD_VIDEO_CLASS=y (Bulk, aktiv)│   │
│  │  ├── usbd_uvc_iso.c (ISO Fork — next step)  │   │
│  │  ├── udc_dwc2.c (ISO bereits vorhanden)     │   │
│  │  DCMIPP → AXISRAM Ring Buffer               │   │
│  │  Ethernet/RTSP parallel                     │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### Zielbild Datenpfad

```
Sensor (ATTO640D) → CPLD (MachXO2) → AD9649 ADC → DCMIPP → AXISRAM Ring Buffer → Frame Dispatcher ─┬→ Zephyr UVC ISO (USB HS)
                                                                                                      └→ Ethernet RTSP (Zephyr)
                                                                                                      └→ VENC H.264 (optional, später)
```

### Aktueller Stand (Phase C — DCMIPP→UVC Streaming funktioniert ✅)

```
ATTO640D → AD9649 → CPLD → DCMIPP Pipe0 → AXISRAM1 (Double-Buffer)
    → memcpy → AXISRAM3 (UVC Buffer) → USB Bulk HS → Windows UVC
```

~19-22 FPS bei 640×480 YUYV. Windows Kamera-App zeigt echte Infrarotbilder.
DCMIPP liefert 52 FPS — USB Bulk ist der Bottleneck (max ~14 MB/s unter Last).

### Nächster Schritt (Phase ISO-1 — Fork aktivieren)

`usbd_uvc_iso.c` in `CMakeLists.txt` einbinden, `CONFIG_USBD_VIDEO_CLASS=n` setzen.
Ziel: ISO High-Bandwidth (3×1024 = 24,6 MB/s) → 30 FPS erreichbar.

### USB UVC Streaming — Kritische Erkenntnisse

**`video_import_buffer()` ist PFLICHT für externe Buffer (Zephyr 4.3):**

`video_enqueue()` verwendet intern `&video_buf[buf->index]`, NICHT den übergebenen
Buffer direkt. Ohne `video_import_buffer()` ist `buf->index = 0` (BSS-Default) und
`video_buf[0].buffer = NULL` → UVC-Treiber springt auf Adresse 0x00000000 → Hard Fault.

```c
// FALSCH — video_enqueue ignoriert uvc_buf.buffer!
uvc_buf.buffer = (uint8_t *)0x34200000;
video_enqueue(uvc_dev, &uvc_buf);  // ← verwendet video_buf[0].buffer = NULL

// RICHTIG — Buffer in video_buf[] registrieren:
uint16_t idx;
video_import_buffer((uint8_t *)0x34200000, fmt.size, &idx);
uvc_buf.index = idx;   // video_buf[idx].buffer = 0x34200000 ✓
video_enqueue(uvc_dev, &uvc_buf);
```

Header: `#include <zephyr/video/video.h>` (nicht `<zephyr/drivers/video.h>`).

**UVC Buffer-Adressen:**
- `0x34200000` (AXISRAM3, 614.400 B) + `0x34296000` (AXISRAM3+4, 614.400 B)
- NICHT in PSRAM — DWC2 USB-DMA braucht AXI-Bus-erreichbaren Speicher (RISAF fehlt)

---

## Verzeichnisstruktur

```
xi640-workspace/
├── CLAUDE.md                         # ← Dieses File (Single Source of Truth)
├── .claude/
│   ├── skills/                       # Claude Code Skills
│   │   ├── zephyr-stm32-coding/     # Codierstandards
│   │   ├── tdd-embedded/            # Test-Driven Development
│   │   ├── code-review-embedded/    # Review-Checkliste
│   │   └── hw-feedback-loop/        # Hardware-Test-Iteration
│   └── agents/                       # Claude Code Subagents
│       ├── architect.md
│       ├── tdd.md
│       ├── reviewer.md
│       └── phase-tracker.md
├── app/                              # Zephyr Application
│   ├── CMakeLists.txt                # Aktiv: src/main.c + src/lynred_atto640d.c, Projekt: xi640_uvc_bulk
│   ├── prj.conf                      # ⚠️ UTF-8 ohne BOM!
│   ├── west.yml                      # Manifest: nur Zephyr (kein USBX/ThreadX mehr)
│   ├── app.overlay                   # sw-generator + uvc DTS-Knoten
│   ├── boards/
│   │   └── stm32n6570_dk_stm32n657xx_fsbl.overlay  # AXISRAM3-6, PSRAM, USART10 (PB9/PD3), ATTO640D GPIOs/I2C/PWM
│   ├── src/
│   │   ├── main.c                    # AKTIV: Zephyr-native Bulk UVC + DCMIPP-Sim
│   │   ├── lynred_atto640d.c/.h      # AKTIV: Sensor-Init (aus Project 17)
│   │   ├── usbd_uvc_iso.c           # ISO Fork (modifiziert, noch nicht kompiliert)
│   │   └── xi640_st_test.c/.h       # USBX-Ära — Dead Code (ST-ISR Klon, nicht kompiliert)
│   ├── inc/
│   │   └── xi640_uvc_stream.h        # USBX-Ära — Dead Code, nicht kompiliert
│   ├── usbx_zephyr_port/             # USBX-Ära — Dead Code, nicht kompiliert
│   ├── tests/                        # Host-Tests (native_posix)
│   └── docs/                         # Projektdokumentation
│       ├── architecture.md
│       ├── memory_layout.md
│       ├── usb_dma_cache_rules.md
│       ├── option1_iso_uvc_plan.md   # ISO Implementierungsplan
│       └── phase_status.md           # ← Living Document!
└── x-cube-n6-camera-capture-main/    # ST Referenzprojekt (lokal vorhanden)
```

---

## Getroffene Grundsatzentscheidungen

### Architektur ✅
- [x] USB Stack: **Zephyr device_next exklusiv** — kein USBX mehr (`CONFIG_USB_DEVICE_STACK_NEXT=y`)
- [x] USB Mode Ziel: **ISO High-Bandwidth** — 24,6 MB/s theoretisch vs. 14 MB/s Bulk-Ceiling
- [x] USB Mode Aktuell: **Bulk** (Baseline für stabile Plattform) — dann Fork auf ISO upgraden
- [x] Baseline validiert: Bulk UVC + DCMIPP-Simulation läuft stabil (Phase B ✅)
- [x] ISO Fork: `usbd_uvc_iso.c` enthält Alt 0/1 + ISO EP + modifizierte uvc_update/transfer Logik
- [x] Zephyr: 4.3.99 (main) — `aps256xxn_obr` Node vorhanden
- [x] Build: `//fsbl` — Code in AXISRAM → SW-Breakpoints
- [x] Debugging im Hot Path: **RTEdbg oder binäres Tracing** — kein printk/LOG_* im Streaming-Pfad
- [x] west.yml: Nur Zephyr (`import: true`, `revision: main`) — keine USBX/ThreadX-Module

### Memory ✅
- [x] Hot Path (AXISRAM): ISR-Handler, DMA Descriptors, USB ISO Buffer, VENC, Kernel-Objekte
- [x] Cold Path (PSRAM): Netzwerk-Buffer, JPEG/H.264 Output, Logging, große Lookup-Tables
- [x] **Keine ISO-TX-Buffer in PSRAM** — XSPI Latenz zerstört ISO Timing
- [x] Cache-Line Alignment: 32 Byte (Cortex-M55)
- [x] PSRAM @ 100 MHz (200 MHz instabil): ~109 MB/s Write, ~202 MB/s Read

### USB Performance (Ziel) ✅
- [x] `wMaxPacketSize = 0x1400` (1024 × 3 Transactions, High-Bandwidth ISO)
- [x] Transfer-Größe: 3072 Bytes/Mikroframe (MPS), Frame-Chunking im Fork implementiert
- [x] `dwMaxPayloadTransferSize = 3072` (MPS, nicht frame_size!)
- [x] SOF-Synchronisation: udc_dwc2.c handhabt Even/Odd Frame intern

### ISO Fork Erkenntnisse ✅ (aus Phase 3/4)
- [x] `uvc_update()` MUSS `uvc_flush_queue(dev)` aufrufen wenn `alternate == 1`
- [x] `dwMaxPayloadTransferSize`: 3072 (MPS) — nicht `frame_size + header` (614655 = falsch)
- [x] `uvc_continue_transfer()`: auf MPS begrenzen — `net_buf->len` ist uint16_t (max 65535)
- [x] ZLP (`bi->udc.zlp`): `false` für ISO — ZLP ist ein Bulk-Konzept

### 3 No-Gos (aus Erfahrung bestätigt) 🚫
1. **Zephyr USB + USBX gleichzeitig** → Geisterbugs, IRQ-Konflikte (erlebt: CDC+UVC Konflikt)
2. **ISO Buffer in PSRAM ohne Cache-Konzept** → "Läuft 10 Sekunden, dann tot"
3. **Pipeline zu früh koppeln** → 5 Unbekannte gleichzeitig (mehrfach erlebt)

---

## Memory Layout

| Region | Adresse | Größe | Verwendung |
|--------|---------|-------|------------|
| AXISRAM1 | 0x34000000 | 2 MB | Zephyr SRAM (Heap, Stacks) |
| AXISRAM2 | 0x34180400 | 511 KB | FSBL Code + BSS |
| AXISRAM3 | 0x34200000 | 512 KB | Video Frame Buffer A + B + ISO TX |
| AXISRAM4 | 0x34280000 | 512 KB | Video Frame Buffer B |
| AXISRAM5 | 0x34300000 | 512 KB | VENC Input Buffer |
| AXISRAM6 | 0x34380000 | 512 KB | VENC Output / Netzwerk |
| VENC RAM | 0x34400000 | 256 KB | H.264 Encoder intern |
| PSRAM | 0x90000000 | 32 MB | Große Buffer, Netzwerk |

### USB ISO Buffer Layout (AXISRAM3 — für ISO Phase)

```
0x34200000 + 0x000000  Frame Buffer Ping  (614.400 B = 0x96000)
0x34200000 + 0x096000  Frame Buffer Pong  (614.400 B = 0x96000)
0x34200000 + 0x12C000  UDC ISO TX Buffer  (wird vom Zephyr UDC verwaltet)
```

### Cache-Regeln (Cortex-M55)

```c
// VOR DMA-Transfer: Cache flush (CPU → Memory)
sys_cache_data_flush_range(buf, size);    // TX

// NACH DMA-Transfer: Cache invalidate (Memory → CPU)
sys_cache_data_invd_range(buf, size);     // RX

// Alignment: 32 Byte (Cache-Line Größe)
__attribute__((aligned(32))) uint8_t dma_buf[SIZE];
```

---

## Thread-Prioritäten (Zephyr-native)

| Thread / ISR | Priorität | Stack | Zweck |
|---|---|---|---|
| USB ISR (OTG HS / DWC2) | ISR | — | SOF-Interrupt, UDC Events |
| DCMIPP ISR | ISR | — | Frame-Capture darf nicht stallen |
| UDC STM32 Thread | konfigurierbar | CONFIG_UDC_STM32_STACK_SIZE=2048 | USB Device Controller Events |
| Streaming (main/loop) | Kooperativ mit Timer | CONFIG_MAIN_STACK_SIZE=4096 | video_enqueue/dequeue Loop |
| DCMIPP DMA / Dispatcher | (spätere Phase) | 2048 | Echte Kamera-Integration |
| Ethernet TX | (spätere Phase) | 4096 | RTSP Streaming |

⚠️ Keine Logs im Hot Path — keine Mutexe in ISR — kein Heap während Streaming.

---

## USB Bandwidth-Rechnung

```
Frame:      640 × 480 × 2 = 614.400 Bytes
30 FPS:     614.400 × 30  = 18,43 MB/s  (Ziel)
USB HS ISO: 1024 × 3 × 8  = 24,58 MB/s (Maximum)
Reserve:    6,15 MB/s (25% Puffer) ✅

Bulk HS:    512 × N        = max ~14 MB/s (unter Last) — daher ISO Ziel
```

---

## Phasenstatus

> ⚠️ `app/docs/phase_status.md` ist das Living Document — nach jeder Phase dort aktualisieren!

| Phase | Titel | Status | Datum |
|-------|-------|--------|-------|
| A0 | USBX: Architektur fixieren | ✅ Abgeschlossen (aufgegeben) | 2026-03 |
| A1 | USBX: Zephyr Port-Layer | ✅ Abgeschlossen (aufgegeben) | 2026-03 |
| A2 | USBX: STM32N6 DCD + HS PHY | ⛔ Aufgegeben (xact errors) | 2026-03 |
| **B** | **Zephyr Bulk Baseline + DCMIPP-Sim** | **✅ Abgeschlossen** | **2026-03** |
| **C** | **DCMIPP→UVC echte Thermaldaten** | **✅ Abgeschlossen** | **2026-03-29** |
| **ISO-1** | **ISO Fork aktivieren** | **🔄 Nächster Schritt** | — |
| ISO-2 | Hardware-Test: ISO Enumeration | ⏳ Offen | — |
| ISO-3 | Hardware-Test: ISO Streaming (Colorbar) | ⏳ Offen | — |
| ISO-4 | DCMIPP Integration (echte Kamera) | ⏳ Offen | — |
| ISO-5 | Integration + FPS Test + Produktisierung | ⏳ Offen | — |

### Erkenntnisse aus abgeschlossenen Phasen

**USBX-Ära (A0–A2):**
- FSBL-Variante (`//fsbl`) nötig für AXISRAM → SW-Breakpoints
- Overlay muss `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` heißen
- prj.conf ohne BOM speichern
- AXISRAM-Bänke über originale DTS-Knoten (`&axisram3 { status = "okay"; }`) aktivieren
- USB OTG HS PHY: VDD USB Supply + PHY Reference Clock (USBPHYC_CR FSEL=0x2)
- USBX aufgegeben: USB HS IN-Transfer xact errors trotz RIFSC-Fix, DSB/ISB, k_sem-Pattern
- Dead Code (USBX-Ära, vorhanden aber nicht kompiliert): `usbx_zephyr_port/`, `xi640_dcd.c`, `usbx_zephyr.c`, `xi640_uvc_stream.c`, `xi640_st_test.c` (ST-ISR-Klon Diagnose), `Xi640_USBX_Hybrid_Architecture.md` (veraltete Architekturdoku)

**Phase B — Zephyr Bulk Baseline:**
- `CONFIG_USBD_VIDEO_CLASS=y` + `CONFIG_VIDEO_SW_GENERATOR=y` + `app.overlay` (sw-generator + uvc Knoten)
- Projekt-Name in CMakeLists.txt: `xi640_uvc_bulk`
- DCMIPP-Simulation: `k_timer` @ **25 ms (~40 FPS)** gibt `k_sem` → main-Loop `video_dequeue/enqueue`
  *(30 FPS wäre 33 ms — `DCMIPP_SIM_PERIOD_MS` in main.c anpassen falls nötig)*
- `VIDEO_BUFFER_POOL_SZ_MAX=614400`, `VIDEO_BUFFER_POOL_NUM_MAX=8`
- `UDC_BUF_POOL_SIZE=32768` (USB DMA-Buffer Pool)
- `UDC_STM32_MAX_QMESSAGES=64` wichtig für ISO (später)
- Benchmark läuft: alle 30 Frames FPS + kbps in LOG_INF ausgegeben
- Console: USART10 (PB9=TX, PD3=RX, 115200 Baud) — USART1 disabled ✅ (2026-03-20)
- Lynred ATTO640D-04 integriert: `lynred_atto640d.c/.h` aus Project 17, Build OK ✅ (2026-03-20)
- ✅ Windows Hardware-Test: USB-Videogerät sichtbar, Windows Kamera-App zeigt Bild (2026-03-29)

**Phase C — DCMIPP→UVC echte Thermaldaten (2026-03-29):**
- Root Cause Hard Fault: `video_import_buffer()` fehlte → `video_buf[0].buffer = NULL` → Adresse 0 → Fault
- `video_enqueue()` verwendet `&video_buf[buf->index]`, NICHT den übergebenen Buffer
- Fix: `video_import_buffer(ptr, size, &idx)` + `buf.index = idx` vor jedem `video_enqueue()`
- Header `<zephyr/video/video.h>` nötig (nicht `<zephyr/drivers/video.h>`)
- RIMC: OTG1 (Index 4) + OTG2 (Index 5) müssen CID1 Secure+Priv haben (wie DCMIPP)
- Security: ALLE 16 ITNS-Register löschen (for-Schleife), VTOR_NS auf `SCB->VTOR & ~(1<<28)`
- Kconfig: `-DCONFIG_FOO=n` überschreibt `default y` nicht zuverlässig → EXTRA_CONF_FILE
- Diagnose-Build: `diag_swgen.conf` für SW-Generator A/B-Test vorhanden
- Performance: ~19-22 FPS bei 640×480 YUYV unkomprimiert, 1000+ Frames stabil

**ISO Fork (Phase 3/4 — Code vorhanden, noch nicht aktiviert):**
- `usbd_uvc_iso.c`: Fork von Zephyr `usbd_uvc.c` mit Alt 0 (kein EP) + Alt 1 (ISO EP `0x1400 = 3×1024`)
- `uvc_update()` MUSS `uvc_flush_queue(dev)` aufrufen wenn `alternate == 1` — ohne dies keine Daten
- `dwMaxPayloadTransferSize`: 3072 (MPS) — NICHT `frame_size + header`
- `uvc_continue_transfer()`: auf MPS begrenzen (uint16_t Overflow-Schutz)
- ZLP = false für ISO
- **Header-Includes im Fork:** `#include "usbd_uvc.h"` / `"video_ctrls.h"` / `"video_device.h"` (LOKAL, nicht absolut)
  → CMakeLists.txt: `zephyr_include_directories(${ZEPHYR_BASE}/subsys/usb/device_next/class)`

### Nächste Schritte: Phase ISO-1 — Fork aktivieren

**Vorbedingung:** Bulk Baseline läuft stabil ✅

**Schritte:**
1. `CMakeLists.txt`: `target_sources(app PRIVATE src/usbd_uvc_iso.c)` statt `CONFIG_USBD_VIDEO_CLASS`
2. `prj.conf`: `# CONFIG_USBD_VIDEO_CLASS=n` → durch eigene UVC-Klasse ersetzen
3. `west build --pristine` + Flash
4. Verifizieren: USBTreeView zeigt Alt 0 (0 EPs) + Alt 1 (ISO EP 0x1400) ✅
5. Bus Hound: Probe/Commit + SET_INTERFACE(1) ✅
6. AMCap / Windows Kamera: Colorbar sichtbar

---

## Offene Fragen

### Phase ISO-1 (Fork aktivieren)
- Zephyr-Header für Fork: ✅ Geklärt — lokale `#include "usbd_uvc.h"` etc. → `zephyr_include_directories(${ZEPHYR_BASE}/subsys/usb/device_next/class)` in CMakeLists.txt nötig
- Kconfig: Eigene `Kconfig`-Symbole für Fork nötig oder nur `CONFIG_USBD_VIDEO_*` nutzen?

### Phase ISO-2 (Hardware)
- Host: Windows 10/11, Standard-UVC-Treiber (usbvideo.sys)
- Testpattern: Statisch (YUYV 0x80 + Frame-Counter) — für FPS-Verifikation reicht das

### Phase ISO-4 (DCMIPP)
- Thomas' DCMIPP Code-Stand? API-Grenzen definiert?
- Bus Contention: AXI-Bandbreite reicht für USB + DCMIPP + Ethernet parallel?

---

## Messgrößen (Definition of Done — projektübergreifend)

| Messgröße | Ziel | Methode |
|-----------|------|---------|
| USB Speed | HS (480 Mbit/s), nicht FS (12 Mbit/s) | Windows Gerätemanager |
| UVC FPS | 30 FPS ± 1 | Host-Tool (OBS, ffplay, USB Analyser) |
| Effektive Bandbreite | ~18 MB/s | USB Analyser / Byte-Counter |
| Drop Counter | < 1% über 5 Minuten | Firmware-interner Zähler |
| ISO Incomplete Events | Minimal | OTG HS Register GINTSTS |
| Reconnect-Stabilität | 10× Reconnect ohne Absturz | Manueller Test |
| Heap-Stabilität | Kein Leak nach 60 Minuten Streaming | Zephyr Heap Stats |
| CPU-Last | < 50% im Streaming | Zephyr Thread Analyzer |

---

## Lynred ATTO640D-04

Bolometer-Sensor, integriert aus Project 17. Sourcen: `app/src/lynred_atto640d.c/.h`.

### Pin-Mapping

| Signal | GPIO | Funktion |
|--------|------|---------|
| NRST | PC9 | FPA Reset (active-high im Overlay → Active = Reset halten) |
| SEQ_TRIG | PC8 | Sequencer Trigger (Free-run: unbenutzt) |
| AVDD_EN | PE9 | 3.75V Regler ein (ADP7118, IC370) |
| VDDA_EN | PE15 | 1.8V Regler ein (ADP150, IC360) |
| I2C1_SCL | PH9 | I2C1, AF4, Open-Drain — HAL-Registerfix in Code |
| I2C1_SDA | PC1 | I2C1, AF4, Open-Drain — HAL-Registerfix in Code |
| MC (Master Clock) | PC7 | TIM3_CH2, AF2, 33 MHz via Zephyr PWM-Treiber |

### Kritische Erkenntnisse (aus Project 17)

- **GPIOE MODER-Schutz:** PE0,1,3,4,5,7,8,10 = AD9649 ADC-Datenpins — dürfen NIEMALS Output sein. Bus-Konflikt → ADC-IC-Beschädigung. Check+Fix ist in `atto640d_gpio_init()` eingebaut.
- **RIF SECCFGR/PRIVCFGR Clearing** für PE9/PE15 PFLICHT — FSBL läuft im Secure-Modus. Zephyr GPIO-Treiber (Non-Secure Alias) kann Pins sonst nicht steuern.
- **I2C1 HAL Pin-Config** (PH9/PC1 als AF4 Open-Drain): Zephyr pinctrl allein reicht auf STM32N6 ggf. nicht — manuelle Registerkorrektur nach `device_is_ready()` im Code vorhanden.
- **PWM statt Timer-Register-Hack:** `timers3` + `atto640d_mc_pwm` DTS-Knoten → saubere Zephyr PWM API.
- **I2C-Adresse:** 0x12 (I2CAD-Pin auf GND). 16-bit Register-Adressierung (MSB first).
- **Startup-Reihenfolge:** AVDD → 10µs → DVDD → 10ms → MC → NRST-Release → 2ms → I2C ID-Check.
- **I2C2 Bus Recovery** (MachXO2 CPLD): Falls SDA low hält nach JTAG-Programmierung → 9 SCL-Pulse vor erstem I2C2-Zugriff.

### Startup-Sequenz (UG038 Table 33)

```
1. NRST = 0 (Reset aktiv)
2. AVDD_EN = 1 (PE9, 3.75V)
3. 10 µs warten
4. VDDA_EN = 1 (PE15, 1.8V)
5. 10 ms warten (Power stabil)
6. Master Clock starten (TIM3_CH2/PC7, 33 MHz)
7. NRST = 1 (Reset aufheben)
8. 2 ms warten (FPA Boot)
9. I2C ID lesen (0x55/0xC6/0xCA erwartet)
10. CONFIG_B = 0x01 (I2C_DIFF_EN)
11. CONFIG_E = 0x02 (REG_INIT)
12. 2 ms warten
13. 300 ms warten (FPA Init)
14. Free-run Modus: TRIGGER_1=0, TRIGGER_2=0, START_SEQ → STATUS prüfen
```

### Definition of Done — Sensor-Init

| Test | Erwartung |
|------|-----------|
| LOG "ATTO640D: ID OK" | I2C erreichbar, MC läuft |
| LOG "Sequencer aktiv" | STATUS: ROIC_INIT_DONE=1, SEQ_STATUS=0 |
| Kein "ADC-Pin-Konflikt" | GPIOE MODER korrekt |

---

## STM32N6 Spezifische Hinweise

- **VENC braucht DCMIPP Clock** — auch ohne Kamera! `__HAL_RCC_DCMIPP_CLK_ENABLE()`
- **ISR FPU Context** — VENC IRQ immer mit `IRQ_CONNECT` (nicht `ISR_DIRECT_DECLARE`)
- **D-Cache** — muss explizit aktiviert werden (`CONFIG_DCACHE=y`), sonst nur ~15 MB/s
- **Software-Breakpoints** — nur in AXISRAM, nicht in externem Flash (0x70xxxxxx)
- **PSRAM Reset** — nach Power-Cycle: 0xFF Global Reset in SPI-Mode vor HexaSPI-Switch
- **USB OTG HS PHY** — Braucht VDD USB Supply + PHY Reference Clock. Bei Zephyr UDC DWC2: wird vom Treiber/Board-Init gehandhabt (anders als USBX-Ära mit manuellem HAL_PCD_MspInit)
- **AXISRAM-Bänke** — Müssen über originale DTS-Knoten (`&axisram3 { status = "okay"; }`) aktiviert werden. Custom `memory@xxxxx` Knoten triggern RAMCFG-Treiber NICHT.
- **Zephyr Thread-Starvation** — `k_yield()` gibt CPU nur an gleich-/höher-priorisierte Threads ab. In Idle-Loops `k_sleep(K_MSEC(1))` verwenden.
- **RIMC: ALLE DMA-Master konfigurieren** — Jeder DMA-Master der auf AXISRAM3-6 zugreift braucht `HAL_RIF_RIMC_ConfigMasterAttributes()` mit CID1 Secure+Priv. Ohne CID → AXI Bus Error → Hard Fault. Bekannte Master: DCMIPP (Index 3), OTG1 (Index 4), OTG2 (Index 5). Muss in `main()` VOR Peripherie-Nutzung gesetzt werden.
- **Security: ALLE ITNS-Register löschen** — Nicht nur einzelne Register, sondern alle 16 (deckt IRQ 0..511): `for (int i = 0; i < ARRAY_SIZE(NVIC->ITNS); i++) NVIC->ITNS[i] = 0;`. VTOR_NS auf NS-Alias: `SCB_NS->VTOR = SCB->VTOR & ~(1UL << 28)` (0x34xxxxxx → 0x24xxxxxx). Ohne dies: USB-IRQ → CPU wechselt NS → VTOR_NS zeigt auf Secure-Adresse → Bus Fault.
- **Kconfig Bool überschreiben** — `-DCONFIG_FOO=n` auf der west-Commandline überschreibt `default y` NICHT zuverlässig. Stattdessen `EXTRA_CONF_FILE` verwenden: `-- -DEXTRA_CONF_FILE=override.conf` mit `CONFIG_FOO=n` darin.

---

## Debugging-Strategie

### Hot Path (ISO Streaming, ISR, DMA)
- **Kein printk / LOG_*** — verändert Timing, zerstört ISO
- **RTEdbg evaluieren** — binäres Logging in RAM-Ringbuffer, ~35 Zyklen pro Event
- **GPIO-Toggles** — für Timing-Messungen mit Oszilloskop/Logic Analyzer
- **USB Analyser** — für Protokoll-Level Debugging (Wireshark + USBPcap, oder Total Phase)

### Cold Path (Init, Konfiguration, Fehler)
- **printk / LOG_INF** — erlaubt außerhalb des Streaming-Pfads
- **Zephyr Shell** — für Runtime-Inspektion (Heap Stats, Thread Info)
- **Core Dump** — bei Hard Faults, über Zephyr Coredump Subsystem

### Hardware-Feedback-Loop (siehe Skill `hw-feedback-loop`)
- Für Phasen die Hardware-Validierung brauchen (ISO-1+)
- Definierter Zyklus: Hypothese → Flash → Beobachten → Ergebnis melden → Iteration

---

## Konfiguration (aktuell aktiv)

| Datei | Zweck | Hinweise |
|-------|-------|---------|
| `app/prj.conf` | Zephyr Kconfig | UTF-8 ohne BOM! |
| `app/app.overlay` | sw-generator + uvc DTS-Knoten | Neu seit Phase B |
| `app/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` | AXISRAM3-6, PSRAM, USART10 (PB9/PD3) | |
| `app/CMakeLists.txt` | Build-Definition | Minimal: nur src/main.c |
| `app/west.yml` | West Manifest | Nur Zephyr, kein USBX/ThreadX |

---

## Go / No-Go Kriterien

| Kriterium | Go ✅ | No-Go ❌ |
|-----------|-------|----------|
| USB Speed | HS zuverlässig (480 Mbit/s) | Nur Full-Speed (12 Mbit/s) |
| Baseline | Bulk UVC läuft stabil | Instabile Plattform |
| ISO Fork | Alt 0/1 + ISO EP korrekt enumeriert | Bulk EP statt ISO EP |
| Dummy ISO | Colorbar sichtbar in AMCap/OBS | Kein Bild, ISO Incomplete Flood |
| Parallelbetrieb | USB + Ethernet ohne FPS-Chaos | Nicht beherrschbare Bus-Contention |

---

## Referenzen

| Ressource | Zweck |
|-----------|-------|
| `x-cube-n6-camera-capture-main/` | ST Referenzprojekt mit funktionierender USBX UVC ISO Impl. |
| `app/docs/option1_iso_uvc_plan.md` | Detaillierter ISO Fork Implementierungsplan |
| `zephyr/subsys/usb/device_next/class/usbd_uvc.c` | Original-Quelle des ISO Forks |
| `zephyr/subsys/usb/device_next/class/usbd_uac2.c` | Referenz für ISO EP Handling in Zephyr device_next |

---

## Agent-Orchestrierung

### Verfügbare Agents

| Agent | Aufgabe | Model | Wann einsetzen |
|-------|---------|-------|----------------|
| `architect` | Architektur, Interfaces, State Machines | Opus | Neue Module, Schnittstellen-Design |
| `tdd` | Test-First Entwicklung | Sonnet | Jede neue Funktion |
| `reviewer` | Code-Review nach Checkliste | Sonnet | Vor jedem Commit |
| `phase-tracker` | Dokumentation synchron halten | Sonnet | Nach jeder Phase |

### Verfügbare Skills

| Skill | Triggert automatisch bei... |
|-------|---------------------------|
| `zephyr-stm32-coding` | C-Code mit Zephyr-APIs, STM32-HAL, DMA, Cache, USB |
| `tdd-embedded` | Test-Erstellung, Testpläne, TDD-Workflows |
| `code-review-embedded` | Code-Reviews, Pull Requests, Qualitätsprüfung |
| `hw-feedback-loop` | Hardware-Tests, Target-Validierung, Debugging auf HW |

---

## Lizenz

- Zephyr: Apache-2.0
- Projektcode: Proprietary — Optris GmbH
