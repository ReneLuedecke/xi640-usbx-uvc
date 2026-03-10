---
name: hw-feedback-loop
description: Hardware-Test und Validierungs-Workflow für Embedded Firmware. Aktiviere wenn Code auf echtem Target getestet werden muss, bei Hardware-Debugging oder wenn Testergebnisse vom Board zurückgemeldet werden.
---

# Hardware-Feedback-Loop — Xi640

## Warum dieser Skill existiert

Claude Code kann keinen Code auf dem STM32N6570 flashen oder debuggen.
Aber Claude Code kann den **gesamten Zyklus drumherum** steuern:
Hypothese formulieren → Code generieren → Build prüfen → Du flasht → Du beobachtest → Du meldest Ergebnis → Claude Code iteriert.

## Der Zyklus

```
┌──────────────────────────────────────────────┐
│  1. HYPOTHESE (Claude Code)                   │
│     "FIFO mit 2048 Words für EP1 sollte      │
│      reichen für High-Bandwidth ISO"          │
├──────────────────────────────────────────────┤
│  2. CODE (Claude Code)                        │
│     Generiert/ändert Code, prüft Build        │
│     `west build -d build/primary`             │
├──────────────────────────────────────────────┤
│  3. FLASH + BEOBACHTEN (Du)                   │
│     `west flash -d build/primary`             │
│     Prüfe: Gerätemanager, OBS, Serial Log     │
├──────────────────────────────────────────────┤
│  4. ERGEBNIS MELDEN (Du → Claude Code)        │
│     Nutze die Checkliste unten                │
├──────────────────────────────────────────────┤
│  5. ITERATION (Claude Code)                   │
│     Analysiert Ergebnis, passt Code an        │
│     → Zurück zu Schritt 2                     │
└──────────────────────────────────────────────┘
```

## Ergebnis-Checklisten (Copy-Paste in Chat)

### Phase 4 — USB Enumeration

```
USB Enumeration Ergebnis:
- Windows Gerätemanager: [HS 480Mbit / FS 12Mbit / Nicht erkannt]
- Gerätename: [was steht da?]
- Fehler im Boot-Log: [ja/nein, wenn ja: einfügen]
- Reconnect getestet: [X mal, stabil / instabil]
- ST-LINK Debug: [hält bei main / hängt / crasht bei ___]
```

### Phase 5 — UVC Dummy-Stream

```
UVC Stream Ergebnis:
- Windows Kamera-App: [Bild sichtbar / schwarzes Bild / App crasht]
- OBS/ffplay: [FPS Anzeige: ___]
- Stream-Dauer: [Sekunden/Minuten bis Abbruch, oder stabil]
- Farbbalken: [korrekte Farben / verzerrt / verschoben]
- Boot-Log Auffälligkeiten: [einfügen wenn vorhanden]
- Wireshark USB-Capture: [wenn verfügbar: ISO Pakete sichtbar?]
```

### Phase 6 — DCMIPP Pipeline

```
DCMIPP Ergebnis:
- Sensor erkannt: [ja/nein]
- Frame in AXISRAM: [verifiziert per Debugger / nicht geprüft]
- Frame-Rate DCMIPP: [gemessen: ___ FPS]
- USB Stream mit echtem Bild: [ja / nur Rauschen / kein Bild]
- CPU-Last: [Zephyr Thread Analyzer: ___%]
```

### Allgemein — Crash / Hard Fault

```
Crash-Report:
- Stelle: [wo im Code, falls bekannt]
- Fault-Typ: [HardFault / BusFault / UsageFault / unbekannt]
- Register (falls aus Debug): [PC=0x___, LR=0x___, CFSR=0x___]
- Reproduzierbar: [immer / manchmal / einmalig]
- Letzte Änderung: [was wurde zuletzt geändert?]
```

## Tipps für effektives Feedback

**Gut:**
```
"Windows Gerätemanager zeigt 'USB-Verbundgerät' unter USB 2.0 Hub.
 Kein UVC Gerät. Boot-Log: 'DCD: FIFO overrun on EP1'.
 Passiert bei jedem Reconnect."
```

**Schlecht:**
```
"Geht nicht."
```

Je präziser dein Feedback, desto schneller findet Claude Code die Lösung.

## Debugging-Tools auf dem Host

| Tool | Zweck | Wann |
|------|-------|------|
| Windows Gerätemanager | USB Speed (HS/FS), Gerätename | Phase 4+ |
| Windows Kamera-App | UVC Stream visuell prüfen | Phase 5+ |
| OBS Studio | FPS-Messung, Stream-Stabilität | Phase 5+ |
| ffplay / ffprobe | FPS, Format, Codec verifizieren | Phase 5+ |
| Wireshark + USBPcap | USB-Protokoll-Level Analyse | Phase 4+ |
| Zephyr Shell (UART) | Heap Stats, Thread Info, Runtime | Alle Phasen |
| Logic Analyzer | GPIO-Timing, ISR-Dauer | Phase 4+ |
| ST-LINK + GDB | Register, Breakpoints, Memory | Alle Phasen |
