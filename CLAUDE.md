# CLAUDE.md

Dieses File gibt Claude Code den vollständigen Projektkontext.
Sprache: **Deutsch** (Docs, Kommentare, Commits).

---

## Projektübersicht

Xi640 ist Firmware für eine Optris Thermalkamera auf dem STM32N6570-DK (Cortex-M55).
**Hybrid-Architektur:** Zephyr RTOS für Systemdienste, USBX (Eclipse ThreadX, MIT) exklusiv für USB OTG HS — UVC Isochronous Streaming 640×480 YUYV @ 30 FPS.

| Eigenschaft | Wert |
|---|---|
| MCU | STM32N6570 (Cortex-M55) |
| Board | STM32N6570-DK |
| RTOS | Zephyr 4.3.99 (main branch) |
| USB Middleware | USBX (stm32-mw-usbx v6.4.0, SHA `280f6bc`) |
| ThreadX | eclipse-threadx v6.5.0 (SHA `3726d790`) |
| Build-Variante | `//fsbl` — Code in AXISRAM → SW-Breakpoints |
| Zielformat | 640×480 YUYV @ 30 FPS = 18,43 MB/s |
| USB-Modus | ISO High-Bandwidth (3×1024 = 24,6 MB/s max) |
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

# Flash
west flash -d build/primary

# Host-Tests (native_posix, ohne Hardware)
west build -b native_posix app/tests -d build/test
./build/test/zephyr/zephyr.exe
```

⚠️ Externer Flash (0x70xxxxxx) unterstützt keine SW-Breakpoints.

---

## Architektur — Zwei Subsysteme, eine MCU

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

### Zielbild Datenpfad

```
Sensor → DCMIPP → AXISRAM Ring Buffer → Frame Dispatcher ─┬→ USBX UVC ISO (USB HS)
                                                           └→ Ethernet RTSP (Zephyr)
                                                           └→ VENC H.264 (optional, später)
```

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
│       ├── architect.md              # Architektur + Interface
│       ├── tdd.md                    # Red-Green-Refactor
│       ├── reviewer.md              # Code-Review
│       └── phase-tracker.md         # Doku synchron halten
├── app/                              # Zephyr Application
│   ├── CMakeLists.txt
│   ├── prj.conf                      # ⚠️ UTF-8 ohne BOM!
│   ├── west.yml                      # Manifest mit gepinnten SHAs
│   ├── boards/
│   │   └── stm32n6570_dk_stm32n657xx_fsbl.overlay
│   ├── src/
│   │   └── main.c
│   ├── usbx_zephyr_port/             # USBX Source-Integration (add_subdirectory)
│   ├── tests/                        # Host-Tests (native_posix)
│   └── docs/                         # Projektdokumentation
│       ├── architecture.md
│       ├── memory_layout.md
│       ├── usb_dma_cache_rules.md
│       └── phase_status.md           # ← Living Document!
├── modules/                          # west update → gitignored
│   ├── st_usbx/                      # STM32CubeN6 USBX Vendor Snapshot
│   ├── threadx/                      # Eclipse ThreadX
│   ├── hal/stm32/                    # STM32 HAL Driver
│   └── usbx_zephyr_port/            # Port-Layer als Zephyr-Modul
└── zephyr/                           # Zephyr Kernel
```

---

## Getroffene Grundsatzentscheidungen

### Architektur ✅
- [x] USB Mode: **ISO first** — 24,6 MB/s theoretisch vs. 14 MB/s Bulk-Ceiling
- [x] USB Owner: **USBX exklusiv** — kein Zephyr USB Stack (`CONFIG_USB_DEVICE_STACK=n`)
- [x] USBX Quelle: stm32-mw-usbx v6.4.0 — ST DCD bereits integriert
- [x] ThreadX: eclipse-threadx v6.5.0 — MIT Lizenz, USBX-Basis
- [x] Zephyr: 4.3.99 (main) — `aps256xxn_obr` Node vorhanden
- [x] Build: `//fsbl` — Code in AXISRAM → SW-Breakpoints
- [x] Compile-Definitionen: `UX_STANDALONE`, `UX_DEVICE_SIDE_ONLY`
- [x] Debugging im Hot Path: **RTEdbg oder binäres Tracing** — kein printk/LOG_* im Streaming-Pfad

### Memory ✅
- [x] Hot Path (AXISRAM): ISR-Handler, DMA Descriptors, USB ISO Buffer, VENC, Kernel-Objekte
- [x] Cold Path (PSRAM): Netzwerk-Buffer, JPEG/H.264 Output, Logging, große Lookup-Tables
- [x] **Keine ISO-TX-Buffer in PSRAM** — XSPI Latenz zerstört ISO Timing
- [x] Cache-Line Alignment: 32 Byte (Cortex-M55)
- [x] PSRAM @ 100 MHz (200 MHz instabil): ~109 MB/s Write, ~202 MB/s Read

### USB Performance ✅
- [x] `wMaxPacketSize = 0x1400` (1024 × 3 Transactions, High-Bandwidth ISO)
- [x] Transfer-Größe: 16–32 KB Blöcke (nicht 512 B)
- [x] `dwMaxPayloadTransferSize = 24576` (nicht frame_size!)
- [x] OTG HS FIFO: RX=1024, EP0 TX=512, EP1 ISO TX=2048 Words (3584/4096 belegt)
- [x] DMA: GAHBCFG mit DMAEN + INCR16 Burst
- [x] SOF IRQ: Even/Odd Frame Sync auf DIEPCTL1

### Port-Layer ✅
- [x] `UX_THREAD`: `_ux_thread_start_ctx` eingebettet — kein `k_malloc` im Port-Layer
- [x] Thread-Lifecycle: `k_thread_suspend/resume` — `k_sleep/k_wakeup` verboten
- [x] Timeout-Mapping: `UX_WAIT_FOREVER` → explizit `K_FOREVER`, nicht `K_MSEC(0xFFFFFFFF)`
- [x] ISR-Sync: nur `k_sem_give()` aus ISR erlaubt — `k_mutex_lock()` verboten
- [x] Cache: Adressen auf 32-Byte-Cache-Line runden vor `sys_cache_data_flush/invd_range`
- [x] Heap: `UX_BYTE_POOL` → `k_heap`, Größe via `CONFIG_USBX_HEAP_SIZE` (default 65536)
- [x] Port-Layer Verifikation: Target-Build (kein native_posix) — Zephyr-Primitiven nicht mockbar

### 3 No-Gos (aus Erfahrung bestätigt) 🚫
1. **Zephyr USB + USBX gleichzeitig** → Geisterbugs, IRQ-Konflikte (erlebt: CDC+UVC Konflikt)
2. **ISO Buffer in PSRAM ohne Cache-Konzept** → "Läuft 10 Sekunden, dann tot" (erlebt: VENC EWL)
3. **Pipeline zu früh koppeln** → 5 Unbekannte gleichzeitig (mehrfach erlebt)

---

## Port-Layer Kritische Regeln (ux_port.c)

Diese 6 Regeln existieren, weil naive Mappings subtile Bugs verursachen:

| # | Regel | Falsch | Richtig |
|---|-------|--------|---------|
| 1 | Kein k_malloc in Thread-Create | `k_malloc(sizeof(UX_THREAD))` | ctx in UX_THREAD struct einbetten |
| 2 | Thread suspend | `k_sleep(K_FOREVER)` — suspendiert CALLER | `k_thread_suspend(tid)` |
| 3 | Thread resume | `k_wakeup(tid)` — bricht nur k_sleep ab | `k_thread_resume(tid)` |
| 4 | Timeout-Mapping | `K_MSEC(0xFFFFFFFF)` | `UX_WAIT_FOREVER → K_FOREVER` |
| 5 | Cache Alignment | unaligned Buffer | `__attribute__((aligned(32)))` |
| 6 | Keine Mutexe aus ISR | `k_mutex_lock()` in ISR | `k_sem_give()` — ISR-safe |

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

### USB ISO Buffer Layout (AXISRAM3)

```
0x34200000 + 0x000000  Frame Buffer Ping  (614.400 B = 0x96000)
0x34200000 + 0x096000  Frame Buffer Pong  (614.400 B = 0x96000)
0x34200000 + 0x12C000  USBX ISO TX Buffer (49.152 B  = 0xC000)
0x34200000 + 0x138000  frei
```

### Cache-Regeln (Cortex-M55)

```c
// VOR DMA-Transfer: Cache flush (CPU → Memory)
SCB_CleanDCache_by_Addr((uint32_t *)buf, size);

// NACH DMA-Transfer: Cache invalidate (Memory → CPU)
SCB_InvalidateDCache_by_Addr((uint32_t *)buf, size);

// Zephyr-Wrapper bevorzugen:
sys_cache_data_flush_range(buf, size);    // TX
sys_cache_data_invd_range(buf, size);     // RX

// Alignment: 32 Byte (Cache-Line Größe)
__attribute__((aligned(32))) uint8_t dma_buf[SIZE];
```

---

## Thread-Prioritäten

| Thread / ISR | Priorität | Stack | Zweck |
|---|---|---|---|
| USB ISR (OTG HS) | ISR | — | SOF-Interrupt darf nicht verhungern |
| DCMIPP ISR | ISR | — | Frame-Capture darf nicht stallen |
| USBX ISO TX | 2 | 4096 | ISO Payload muss pünktlich sein |
| DCMIPP DMA / Dispatcher | 3 | 2048 | Direkt nach USBX |
| VENC H.264 | 5 | 8192 | Encoding |
| Ethernet TX | 6 | 4096 | RTSP Streaming |
| Main / App | 10 | 8192 | System Control |
| Zephyr SWQ | 14 | 4096 | System Workqueue |

⚠️ Keine Logs im Hot Path — keine Mutexe in ISR — kein Heap während Streaming.

---

## USB Bandwidth-Rechnung

```
Frame:      640 × 480 × 2 = 614.400 Bytes
30 FPS:     614.400 × 30  = 18,43 MB/s  (Ziel)
USB HS ISO: 1024 × 3 × 8  = 24,58 MB/s (Maximum)
Reserve:    6,15 MB/s (25% Puffer) ✅
```

---

## Phasenstatus

> ⚠️ `app/docs/phase_status.md` ist das Living Document — nach jeder Phase dort aktualisieren!
> Nutze den `phase-tracker` Agent um alle Dokumente synchron zu halten.

| Phase | Titel | Status | Datum |
|-------|-------|--------|-------|
| 0 | Architektur fixieren | ✅ Done | 2026-03 |
| 1 | Zephyr USB deaktivieren + Heartbeat | ✅ Done | 2026-03 |
| 2 | USBX west-Module einbinden | ✅ Done | 2026-03 |
| 3 | USBX Zephyr Port-Layer | ✅ Done | 2026-03 |
| 4 | STM32N6 DCD + HS PHY + DMA + Cache | 🔄 Next | — |
| 5 | UVC ISO Dummy-Stream | ⏳ Offen | — |
| 6 | DCMIPP RAW-Pipeline | ⏳ Offen | — |
| 7 | Parallelbetrieb USB + Ethernet | ⏳ Offen | — |
| 8 | Produktisierung | ⏳ Offen | — |

### Erkenntnisse aus abgeschlossenen Phasen

**Phase 1:** FSBL-Variante (`//fsbl`) nötig für AXISRAM → SW-Breakpoints. Overlay muss `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` heißen. prj.conf ohne BOM speichern. Zephyr 4.1.0 hatte `aps256xxn_obr` Node noch nicht.

**Phase 2:** eclipse-threadx nutzt `master` als Hauptbranch (nicht `main`). Remote-Name in west.yml muss exakt `eclipse-threadx` sein. Module in `.gitignore` — nicht eingecheckt.

**Phase 3:** `_ux_thread_start_ctx` in `UX_THREAD` einbetten (kein `k_malloc` im Hot Path). `k_thread_suspend/resume` sind zwingend — `k_sleep(K_FOREVER)` suspendiert den Aufrufer, nicht den Ziel-Thread. `k_wakeup` setzt keinen suspendierten Thread fort — nur `k_thread_resume`. `UX_WAIT_FOREVER` muss explizit auf `K_FOREVER` geprüft werden. `k_sem_give` ist der einzige ISR-safe USBX-Sync-Aufruf. Cache-Maintenance muss auf 32-Byte-Cache-Line-Grenzen runden, sonst wirkungslos auf Cortex-M55. Host-Tests (native_posix) für Port-Layer nicht realisiert — Zephyr-Kernel-Primitiven nicht mockbar; Build-Verifikation auf Target.

### Nächste Phase: Phase 4 — STM32N6 DCD + HS PHY + DMA + Cache

**Ziel:** USB OTG HS Device Controller Driver (DCD) für STM32N6 aufsetzen — PHY, FIFO, DMA, Cache-Konzept.

**Arbeitspakete:**
- [ ] `ux_dcd_stm32.c` aus CubeN6 analysieren — Anpassungsbedarf N6 vs. H7 klären
- [ ] Clock/PHY Setup für HS (HAL-Calls identifizieren)
- [ ] FIFO-Konfiguration: RX=1024, EP0 TX=512, EP1 ISO TX=2048 Words
- [ ] DMA: GAHBCFG mit DMAEN + INCR16 Burst aktivieren
- [ ] SOF IRQ: Even/Odd Frame Sync auf DIEPCTL1
- [ ] Cache-Konzept: `ux_cache_clean` / `ux_cache_invalidate` in DCD-Transfer-Pfad
- [ ] Build + minimaler Enum-Test (USB HS erkannt, nicht FS)

**Definition of Done:**
- [ ] `west build` kompiliert ohne Fehler
- [ ] Windows Gerätemanager zeigt USB HS (480 Mbit/s), nicht FS
- [ ] Kein Hard Fault bei USB-Enum
- [ ] Git commit + Push

**Offene Fragen:**
- HBSTLEN=INCR16 mit PSRAM: Bei zu hoher XSPI-Latenz auf INCR4 zurückgehen?
- ST DCD aus CubeN6 (`ux_dcd_stm32.c`): Wie viele Anpassungen nötig für N6 vs. H7?
- Clock/PHY Setup: Welche HAL-Calls genau für HS?

---

## Offene Fragen (pro Phase)

### Phase 4 (DCD + HS PHY)
- HBSTLEN=INCR16 mit PSRAM: Bei zu hoher XSPI-Latenz auf INCR4 (0x3) zurückgehen?
- ST DCD aus CubeN6 (ux_dcd_stm32.c): Wie viele Anpassungen nötig für N6 vs. H7?
- Clock/PHY Setup: Welche HAL-Calls genau für HS?

### Phase 5 (UVC Dummy-Stream)
- Ziel-Host: Windows first, dann Linux — aber welche Windows-Version / UVC-Treiber?
- Colorbars-Testpattern: Statisch oder animiert (für FPS-Verifikation)?
- Debugging: RTEdbg evaluieren für ISO-Timing-Analyse ohne printk

### Phase 6+ (Hardware-abhängig)
- DCMIPP Integration: Thomas' Code-Stand? API-Grenzen definiert?
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

## STM32N6 Spezifische Hinweise

- **VENC braucht DCMIPP Clock** — auch ohne Kamera! `__HAL_RCC_DCMIPP_CLK_ENABLE()`
- **ISR FPU Context** — VENC IRQ immer mit `IRQ_CONNECT` (nicht `ISR_DIRECT_DECLARE`)
- **D-Cache** — muss explizit aktiviert werden, sonst nur ~15 MB/s Speicherbandbreite
- **Software-Breakpoints** — nur in AXISRAM, nicht in externem Flash (0x70xxxxxx)
- **PSRAM Reset** — nach Power-Cycle: 0xFF Global Reset in SPI-Mode vor HexaSPI-Switch

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
- Für Phasen die Hardware-Validierung brauchen (Phase 4+)
- Definierter Zyklus: Hypothese → Flash → Beobachten → Ergebnis melden → Iteration

---

## Konfiguration

| Datei | Zweck | Hinweise |
|-------|-------|---------|
| `app/prj.conf` | Zephyr Kconfig | UTF-8 ohne BOM! |
| `app/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` | Board Overlay | PSRAM, USART |
| `app/west.yml` | West Manifest | Gepinnte SHAs für st_usbx + threadx |
| Compile-Defines | `UX_STANDALONE`, `UX_DEVICE_SIDE_ONLY` | In CMakeLists.txt |

---

## Go / No-Go Kriterien

| Kriterium | Go ✅ | No-Go ❌ |
|-----------|-------|----------|
| USB Speed | HS zuverlässig (480 Mbit/s) | Nur Full-Speed (12 Mbit/s) |
| Port-Layer | Klein, stabil, kein Leak | Instabile DMA/Cache Semantik |
| Dummy ISO | Zielbandbreite ~24 MB/s | DCD nur mit massiven CubeMX-Abhängigkeiten |
| Parallelbetrieb | USB + Ethernet ohne FPS-Chaos | Nicht beherrschbare Bus-Contention |

---

## west Module

| Modul | Remote | SHA | Pfad |
|-------|--------|-----|------|
| stm32-mw-usbx | STMicroelectronics | 280f6bc | modules/st_usbx |
| threadx | eclipse-threadx | 3726d790 | modules/threadx |

---

## Agent-Orchestrierung

### Verfügbare Agents

| Agent | Aufgabe | Model | Wann einsetzen |
|-------|---------|-------|----------------|
| `architect` | Architektur, Interfaces, State Machines | Opus | Neue Module, Schnittstellen-Design |
| `tdd` | Test-First Entwicklung | Sonnet | Jede neue Funktion |
| `reviewer` | Code-Review nach Checkliste | Sonnet | Vor jedem Commit |
| `phase-tracker` | Dokumentation synchron halten | Sonnet | Nach jeder Phase |

### Workflow pro Feature (Beningo-Methodik)

```
1. Architect-Agent: Interface definieren (Header + Doku)
         ↓
2. TDD-Agent: Tests schreiben → Code implementieren (Red-Green-Refactor)
         ↓
3. Reviewer-Agent: Code-Review gegen Checkliste
         ↓
4. [Falls Hardware nötig]: hw-feedback-loop Skill → Ergebnis zurückmelden
         ↓
5. Phase-Tracker: Doku aktualisieren, nächste Phase vorbereiten
```

### Parallele Agents

Für unabhängige Arbeitspakete innerhalb einer Phase:

```
Phase 4 Beispiel:
┌─────────────────────────────────────────────┐
│ Parallel:                                    │
│  Agent 1 (TDD): DCD Init + FIFO Tests       │
│  Agent 2 (TDD): PHY Setup + Clock Tests     │
│  Agent 3 (Architect): ISR Handler Design     │
├─────────────────────────────────────────────┤
│ Sequentiell danach:                          │
│  Reviewer: Alle drei Module reviewen         │
│  Phase-Tracker: Phase 4 DoD prüfen          │
└─────────────────────────────────────────────┘
```

### Wann NICHT parallelisieren
- Module voneinander abhängen (DCD Init vor UVC Stream)
- Review eines Moduls Änderungen am anderen erfordert
- Hardware-Tests (nur sequentiell auf einem Target)

### Informationsfluss zu Claude Code

```
┌──────────────┐     Automatisch bei Session-Start
│  CLAUDE.md   │────→ Claude Code weiß: Entscheidungen, Stand, Regeln
└──────────────┘

┌──────────────┐     On-Demand wenn Kontext passt
│   Skills     │────→ Claude Code lädt: Codierstandards, TDD, Review
└──────────────┘

┌──────────────┐     Du sagst es Claude Code
│  HW-Feedback │────→ "Windows zeigt FS statt HS" → Claude iteriert
└──────────────┘

┌──────────────┐     Claude Code liest bei Bedarf
│  app/docs/*  │────→ Detaillierte Referenz (Memory Map, Register, etc.)
└──────────────┘

┌──────────────┐     Claude Code liest Quellcode direkt
│  modules/*   │────→ HAL Header, USBX Source, ThreadX Source
│  zephyr/*    │────→ Zephyr Kernel, Devicetree Bindings
└──────────────┘
```

---

## Verfügbare Skills

| Skill | Triggert automatisch bei... |
|-------|---------------------------|
| `zephyr-stm32-coding` | C-Code mit Zephyr-APIs, STM32-HAL, DMA, Cache, USB |
| `tdd-embedded` | Test-Erstellung, Testpläne, TDD-Workflows |
| `code-review-embedded` | Code-Reviews, Pull Requests, Qualitätsprüfung |
| `hw-feedback-loop` | Hardware-Tests, Target-Validierung, Debugging auf HW |

---

## Lizenz

- Zephyr: Apache-2.0
- USBX (Eclipse ThreadX): MIT
- Projektcode: Proprietary — Optris GmbH
