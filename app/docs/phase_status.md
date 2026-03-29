# Xi640 — Phase Status

> Living document — nach jeder Phase aktualisieren

---

## Übersicht

| Phase | Titel | Status | Datum |
|-------|-------|--------|-------|
| A0 | USBX: Architektur fixieren | ✅ Abgeschlossen (aufgegeben) | 2026-03 |
| A1 | USBX: Zephyr Port-Layer | ✅ Abgeschlossen (aufgegeben) | 2026-03 |
| A2 | USBX: STM32N6 DCD + HS PHY | ⛔ Aufgegeben (xact errors) | 2026-03 |
| **B** | **Zephyr Bulk Baseline + DCMIPP-Sim** | **✅ Abgeschlossen** | **2026-03** |
| **ISO-1** | **ISO Fork aktivieren** | **🔄 Nächster Schritt** | — |
| ISO-2 | Hardware-Test: ISO Enumeration | ⏳ Offen | — |
| ISO-3 | Hardware-Test: ISO Streaming (Colorbar) | ⏳ Offen | — |
| ISO-4 | DCMIPP Integration (echte Kamera) | ⏳ Offen | — |
| ISO-5 | Integration + FPS Test + Produktisierung | ⏳ Offen | — |

---

## USBX-Ära — A0 bis A2 ✅⛔ (aufgegeben 2026-03)

**Warum aufgegeben:** USB HS IN-Transfer xact errors trotz RIFSC-Fix, DSB/ISB, k_sem-Pattern — nicht in vertretbarer Zeit lösbar.

**Erkenntnisse die weiter gelten:**
- FSBL-Variante (`//fsbl`) notwendig damit Code in AXISRAM läuft → SW-Breakpoints funktionieren
- Overlay-Datei muss `boards/stm32n6570_dk_stm32n657xx_fsbl.overlay` heißen
- prj.conf muss ohne BOM (UTF-8 ohne BOM / ASCII) gespeichert werden
- AXISRAM-Bänke über originale DTS-Knoten (`&axisram3 { status = "okay"; }`) aktivieren — Custom `memory@xxxxx` Knoten triggern RAMCFG-Treiber NICHT
- USB OTG HS PHY braucht VDD USB Supply + PHY Reference Clock (USBPHYC_CR FSEL=0x2 für 24 MHz HSE)
- `k_yield()` gibt CPU nur an gleich-/höher-priorisierte Threads ab — Idle-Loops: `k_sleep(K_MSEC(1))`

**Dead Code (vorhanden, nicht kompiliert):**
- `app/usbx_zephyr_port/` — USBX Port-Layer
- `app/src/xi640_dcd.c`, `app/src/usbx_zephyr.c`, `app/src/xi640_uvc_stream.c` — USBX-Ära
- `app/inc/xi640_dcd.h`, `app/inc/usbx*.h`, `app/inc/xi640_uvc_stream.h` — USBX-Ära

---

## Phase B — Zephyr Bulk Baseline + DCMIPP-Sim ✅ (2026-03)

**Ziel:** Stabile Plattform: Windows erkennt UVC-Gerät, Colorbar / DCMIPP-simulierte Frames sichtbar.

**Ergebnis:** Build grün, DCMIPP-Simulation läuft, Benchmark-Ausgabe aktiv.

**Konfiguration:**
```kconfig
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_VIDEO_CLASS=y          # Zephyr Bulk UVC (aktiv)
CONFIG_VIDEO_SW_GENERATOR=y
CONFIG_VIDEO_BUFFER_POOL_NUM_MAX=8
CONFIG_VIDEO_BUFFER_POOL_SZ_MAX=614400
CONFIG_USBD_VIDEO_NUM_BUFS=128
CONFIG_UDC_STM32_MAX_QMESSAGES=64
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SAMPLE_USBD_PID=0x0022
CONFIG_SAMPLE_USBD_PRODUCT="Xi640 UVC Bulk"
CONFIG_SAMPLE_USBD_MANUFACTURER="Optris"
```

**app.overlay (neu):**
```dts
/ {
    chosen { zephyr,camera = &sw_generator; };
    sw_generator: sw-generator {
        compatible = "zephyr,video-sw-generator";
        status = "okay";
    };
    uvc: uvc {
        compatible = "zephyr,uvc-device";
        status = "okay";
    };
};
```

**CMakeLists.txt:**
```cmake
project(xi640_uvc_bulk)
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE src/main.c)
```

**west.yml:** Nur Zephyr (`import: true`, `revision: main`) — kein USBX/ThreadX mehr.

**main.c Architektur (Phase 9: DCMIPP-Simulation):**
- `k_timer` @ 25 ms gibt `k_sem_give()` → simuliert DCMIPP VSYNC
- `video_dequeue()` (K_NO_WAIT) → Frame-Counter schreiben → `video_enqueue()` zurück
- Drop bei USB zu langsam (K_NO_WAIT = echte DCMIPP Semantik)
- Benchmark alle 30 Frames: FPS + kbps in LOG_INF

**Definition of Done:**
- [x] `west build` kompiliert ohne Fehler
- [ ] Windows Gerätemanager zeigt "USB-Videogerät" (UVC) ← **Hardware-Test ausstehend**
- [ ] ffplay / OBS zeigt Bild bei 640×480 ← **Hardware-Test ausstehend**
- [x] Git commit (letzter: `13723d6`)

---

## Phase ISO-1 — ISO Fork aktivieren 🔄 (Nächster Schritt)

**Ziel:** `usbd_uvc_iso.c` ersetzte `CONFIG_USBD_VIDEO_CLASS` — USB Descriptor zeigt Alt 0 (kein EP) + Alt 1 (ISO EP 0x1400).

**Voraussetzung:** Phase B Build läuft stabil ✅

**Arbeitspakete:**
- [ ] `CMakeLists.txt`: `src/usbd_uvc_iso.c` hinzufügen, Zephyr-interne Include-Pfade einbinden
- [ ] `prj.conf`: `CONFIG_USBD_VIDEO_CLASS` deaktivieren (Fork übernimmt)
- [ ] Kconfig-Symbole prüfen: `CONFIG_USBD_VIDEO_*` — werden vom Fork genutzt oder eigene nötig?
- [ ] `west build --pristine` + Flash
- [ ] USBTreeView: Alt 0 (0 EPs) + Alt 1 (ISO EP 0x1400 = 3×1024) ✅
- [ ] Bus Hound: Probe/Commit + SET_INTERFACE(1) bestätigt

**Kritische Punkte im Fork (`usbd_uvc_iso.c`):**
- `uvc_update()` ruft `uvc_flush_queue(dev)` auf wenn `alternate == 1` ✅ (implementiert)
- `dwMaxPayloadTransferSize = 3072` (MPS, nicht frame_size) ✅ (implementiert)
- `uvc_continue_transfer()` begrenzt auf MPS ✅ (implementiert)
- ZLP = false für ISO ✅ (implementiert)

**Interne Zephyr-Header-Abhängigkeiten:**
```
usbd_uvc_iso.c benötigt:
  - zephyr/subsys/usb/device_next/class/usbd_uvc.h     (interne API)
  - zephyr/subsys/usb/device_next/class/video_ctrls.h
  - zephyr/subsys/usb/device_next/class/video_device.h
```
→ `zephyr_include_directories(${ZEPHYR_BASE}/subsys/usb/device_next/class)` in CMakeLists.txt

**Definition of Done:**
- [ ] `west build --pristine` ohne Fehler
- [ ] USBTreeView: ISO Descriptor korrekt (Alt 0 + Alt 1 mit 0x1400)
- [ ] Kein Hard Fault bei Probe/Commit
- [ ] Git commit + Push

---

## Phase ISO-2 — Hardware-Test: ISO Enumeration ⏳

**Ziel:** Windows erkennt UVC mit korrekten ISO Deskriptoren, Host startet ISO-Transfer-Anfragen.

**Erwartetes Verhalten ohne Streaming-Daten:**
- USBTreeView: `wMaxPacketSize = 0x1400`
- Bus Hound: `SET_INTERFACE(1)` → Host sendet ISO-Anfragen → "isoc req failed" (normal ohne Daten)

**Definition of Done:**
- [ ] Windows Gerätemanager: "USB-Videogerät"
- [ ] Host startet ISO Requests nach SET_INTERFACE(1)
- [ ] Kein USB Reset Loop, kein Hard Fault

---

## Phase ISO-3 — Hardware-Test: ISO Streaming (Colorbar) ⏳

**Ziel:** Windows-Kamera-App / AMCap zeigt Colorbar / grau gefülltes YUYV-Bild @ 640×480 @ 30 FPS.

**Definition of Done:**
- [ ] AMCap / Windows Kamera: Bild sichtbar
- [ ] FPS > 20 bei 640×480 YUYV
- [ ] Kein ISO Incomplete Flood (GINTSTS prüfen)
- [ ] 10× Reconnect ohne Absturz
- [ ] Git commit + Push

---

## Phase ISO-4 — DCMIPP Integration (echte Kamera) ⏳

**Ziel:** Echte Kamera-Frames statt DCMIPP-Simulation über USB streamen.

**Offene Fragen:**
- Thomas' DCMIPP Code-Stand? API-Grenzen definiert?
- Bus Contention: AXI-Bandbreite reicht für USB + DCMIPP + Ethernet parallel?

**Definition of Done:**
- [ ] Sensor erkannt
- [ ] Echtes Bild in UVC-Viewer (kein Rauschen, keine Artefakte)
- [ ] Frame-Rate DCMIPP ≥ 30 FPS gemessen
- [ ] CPU-Last < 50% (Zephyr Thread Analyzer)

---

## Phase ISO-5 — Integration + FPS Test + Produktisierung ⏳

**Ziel:** Alle Messgrößen aus DoD erfüllt, stabil über 60 Minuten.

| Messgröße | Ziel |
|-----------|------|
| USB Speed | HS (480 Mbit/s) |
| UVC FPS | 30 FPS ± 1 |
| Effektive Bandbreite | ~18 MB/s |
| Drop Counter | < 1% über 5 Minuten |
| ISO Incomplete Events | Minimal |
| Reconnect-Stabilität | 10× ohne Absturz |
| Heap-Stabilität | Kein Leak nach 60 Min |
| CPU-Last | < 50% |
