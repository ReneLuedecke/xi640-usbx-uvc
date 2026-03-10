# Xi640 — Phase Status

> Living document — nach jeder Phase aktualisieren

## Übersicht

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

---

## Phase 1 — Heartbeat Baseline ✅

**Definition of Done:**
- [x] Build mit `stm32n6570_dk//fsbl` erfolgreich
- [x] Zephyr USB Stack vollständig deaktiviert (`CONFIG_USB_DEVICE_STACK=n`)
- [x] Debugger hält bei `main` (ST-LINK via West Debug Server)
- [x] Kein "usbd" / "UDC" im Boot-Log
- [x] Git commit + Push

**Erkenntnisse:**
- FSBL-Variante (`//fsbl`) notwendig damit Code in AXISRAM läuft → SW-Breakpoints funktionieren
- Overlay-Datei muss `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` heißen
- prj.conf muss ohne BOM (UTF-8 ohne BOM / ASCII) gespeichert werden
- Zephyr 4.3.99 (main branch) — 4.1.0 hatte `aps256xxn_obr` Node noch nicht

---

## Phase 2 — USBX west-Module ✅

**Definition of Done:**
- [x] `stm32-mw-usbx` @ `280f6bc` (v6.4.0_260116) in `modules/st_usbx`
- [x] `threadx` @ `3726d790` (v6.5.0.202601_rel) in `modules/threadx`
- [x] west.yml mit gepinnten SHAs
- [x] `modules/` in `.gitignore` — nicht eingecheckt
- [ ] Build noch grün (prüfen nach CMakeLists-Integration)
- [ ] Git commit + Push

**Erkenntnisse:**
- eclipse-threadx verwendet `master` als Hauptbranch (nicht `main`)
- Remote-Name in west.yml muss exakt dem org-Namen entsprechen: `eclipse-threadx`

---

## Phase 3 — USBX Zephyr Port-Layer ✅ Done (2026-03)

**Ziel:** `ux_port.h` / `ux_port.c` implementieren sodass USBX auf Zephyr-Primitiven läuft

**Arbeitspakete:**
- [x] `ux_port.h` — Typ-Mappings TX_THREAD → k_thread, TX_MUTEX → k_mutex, TX_SEMAPHORE → k_sem, UX_BYTE_POOL → k_heap
- [x] `ux_port.c` — Implementierung aller 5 kritischen Fixes (kein malloc, suspend/resume korrekt, Timeout-Mapping, Cache-Alignment)
- [x] CMakeLists.txt für `modules/usbx_zephyr_port/` (Zephyr-Modul, `CONFIG_USBX_ZEPHYR_PORT`-Guard)
- [x] Kconfig für USBX Module (`CONFIG_USBX_ZEPHYR_PORT`, `CONFIG_USBX_HEAP_SIZE` default 65536)
- [x] Build grün (kein USB noch aktiv)

**Definition of Done:**
- [x] `west build` kompiliert ohne Fehler
- [x] Keine ThreadX-Kernel-Calls im Code — nur Zephyr-Primitiven
- [x] Git commit + Push (e47241c)

**Erkenntnisse:**

- `_ux_thread_start_ctx` direkt in `UX_THREAD` eingebettet — kein `k_malloc` im Hot Path; der Wrapper `zephyr_thread_entry()` holt Entry-Point und Parameter sicher aus dem struct.
- `k_thread_suspend(tid)` / `k_thread_resume(tid)` sind die einzig korrekten Primitiven. `k_sleep(K_FOREVER)` suspendiert den Aufrufer-Thread (den Port-Code selbst), nicht den USBX-Thread — hätte subtile Deadlocks erzeugt.
- `k_wakeup(tid)` bricht nur `k_sleep` ab, setzt keinen suspendierten Thread fort — `k_thread_resume` ist zwingend.
- Timeout-Mapping: `UX_WAIT_FOREVER (0xFFFFFFFF)` muss explizit auf `K_FOREVER` geprüft werden; `K_MSEC(0xFFFFFFFF)` würde ~49 Tage ergeben und wurde vermieden.
- `k_sem_give()` ist ISR-safe — als einzige USBX-Synchronisationsprimitive in ISR-Kontext erlaubt; `k_mutex_lock()` ist es nicht.
- Cache-Maintenance: `cache_aligned()` rundet Adresse auf 32-Byte-Cache-Line-Grenze ab und Größe auf, bevor `sys_cache_data_flush_range` / `sys_cache_data_invd_range` aufgerufen wird — ein unaligned Flush/Invalidate auf dem Cortex-M55 ist wirkungslos.
- `UX_BYTE_POOL` wird auf `k_heap` gemappt — USBX allokiert internen Speicher ausschließlich darüber; `CONFIG_USBX_HEAP_SIZE` (default 65536) bestimmt die Poolgröße.
- Port-Layer liegt unter `modules/usbx_zephyr_port/` (Zephyr-Modul-Pfad); `app/CMakeLists.txt` bindet ihn per `add_subdirectory(usbx_zephyr_port)` ein.
- Host-Tests (native_posix) wurden nicht realisiert — `k_heap`, `k_thread_suspend/resume` sind schwer auf native_posix zu mocken; Verifikation erfolgte über erfolgreichen Target-Build.

**Offene Fragen beantwortet:**

- *USBX Standalone-Modus (`UX_STANDALONE`)*: Thread-Create wird von USBX intern aufgerufen; der Port-Layer muss Thread-Lifecycle vollständig abbilden (nicht entfallen).
- *Heap-Leak-Verifikation ohne Hardware*: Wurde auf Phase 4 verschoben — erst wenn DCD + USB-Stack aktiv, sinnvoll messbar via Zephyr Heap Stats.

---

## Phase 4 — STM32N6 DCD + HS PHY + DMA + Cache 🔄

**Ziel:** USB OTG HS Device Controller fuer STM32N6 aufsetzen — PHY, FIFO, DMA, Cache-Konzept.

**Analyse:** Siehe `phase4_dcd_analysis.md` fuer vollstaendige Ergebnisse.

**Kern-Erkenntnis:** Der USBX DCD Treiber (`ux_dcd_stm32_*.c`) ist familienagnostisch und braucht KEINE Aenderung. Unsere Arbeit liegt in `xi640_dcd.c` — einem Konfigurations- und Glue-Layer zwischen Zephyr, HAL PCD und USBX.

**Arbeitspakete:**
- [x] DCD Treiber-Analyse (Funktionsumfang, Abhaengigkeiten)
- [x] HAL PCD Analyse (STM32N6 vs. H7, PHY, DMA)
- [x] Interface-Entwurf (`xi640_dcd.h`)
- [ ] `xi640_dcd.c` Implementierung:
  - [ ] `PCD_HandleTypeDef` Init (Instance=USB1_OTG_HS, phy=EMBEDDED, dma=1, speed=HS, eps=9, sof=1)
  - [ ] `HAL_PCD_MspInit()` Override (Clock: AHB5 Bit 26 + 28, PHY Controller)
  - [ ] FIFO: `HAL_PCDEx_SetRxFiFo(1024)`, `SetTxFiFo(0, 512)`, `SetTxFiFo(1, 2048)`
  - [ ] DMA Burst: Erstmal INCR4 (HAL Default), spaeter tunen
  - [ ] ISR: `IRQ_CONNECT(177, 2, _ux_dcd_stm32_interrupt_handler, NULL, 0)`
  - [ ] `_ux_dcd_stm32_initialize(0, &hpcd)` Registrierung
  - [ ] Cache-Wrapper (`xi640_dcd_cache_flush/invalidate`)
  - [ ] Diagnostik (`xi640_dcd_get_diagnostics`)
- [ ] CMakeLists.txt: `HAL_PCD_MODULE_ENABLED` + HAL PCD Sources + `UX_DCD_STM32_MAX_ED=9`
- [ ] Build gruen (`west build`)
- [ ] Hardware-Test: Enumeration (Windows Geraetemanager zeigt HS, nicht FS)
- [ ] OTG Core ID (GSNPSID) auslesen + im Boot-Log ausgeben

**Definition of Done:**
- [ ] `west build` kompiliert ohne Fehler
- [ ] Windows Geraetemanager zeigt USB HS (480 Mbit/s), nicht FS
- [ ] Kein Hard Fault bei USB-Enum
- [ ] GSNPSID + Enumeration-Speed im Boot-Log sichtbar
- [ ] Git commit + Push

**Risiken:**
- PHY Controller (`usbphyc1`) braucht moeglicherweise separate Initialisierung
- OTG Core 310A koennte sich anders verhalten als 300A (H7)
- INCR16 DMA Burst koennte auf AXI Interconnect problematisch sein
- Cache-Wrapper muss VOR jedem `HAL_PCD_EP_Transmit()` gerufen werden — vergisst man es, stirbt der Stream nach wenigen Sekunden

**Offene Fragen:**
- Wie initialisiert man den USB PHY Controller (usbphyc1)? Reicht Clock-Enable?
- CubeN6 Beispielprojekte fuer USB Device vorhanden?
- Welchen HBSTLEN Wert empfiehlt ST fuer STM32N6?
- Brauchen wir `USE_HAL_PCD_REGISTER_CALLBACKS=1` oder reichen weak Symbols?
