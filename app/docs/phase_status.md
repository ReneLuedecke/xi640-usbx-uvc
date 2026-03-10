# Xi640 — Phase Status

> Living document — nach jeder Phase aktualisieren

## Übersicht

| Phase | Titel | Status | Datum |
|-------|-------|--------|-------|
| 0 | Architektur fixieren | ✅ Done | 2026-03 |
| 1 | Zephyr USB deaktivieren + Heartbeat | ✅ Done | 2026-03 |
| 2 | USBX west-Module einbinden | ✅ Done | 2026-03 |
| 3 | USBX Zephyr Port-Layer | 🔄 Next | — |
| 4 | STM32N6 DCD + HS PHY + DMA + Cache | ⏳ Offen | — |
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

## Phase 3 — USBX Zephyr Port-Layer 🔄

**Ziel:** `ux_port.h` / `ux_port.c` implementieren sodass USBX auf Zephyr-Primitiven läuft

**Arbeitspakete:**
- [ ] `ux_port.h` — Typ-Mappings TX_THREAD → k_thread, TX_MUTEX → k_mutex, etc.
- [ ] `ux_port.c` — Implementierung aller 6 kritischen Fixes (siehe architecture.md)
- [ ] CMakeLists.txt für `modules/usbx_zephyr_port/`
- [ ] Kconfig für USBX Module
- [ ] Build grün (kein USB noch aktiv)

**Definition of Done:**
- [ ] `west build` kompiliert ohne Fehler
- [ ] Keine ThreadX-Kernel-Calls im Code — nur Zephyr-Primitiven
- [ ] Git commit + Push
