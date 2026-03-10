---
name: code-review-embedded
description: Code-Review für Embedded Firmware nach Optris/Xi640-Standards. Aktiviere bei Code-Reviews, Pull Requests oder wenn Qualitätsprüfung von C-Code angefordert wird.
---

# Code-Review Checkliste — Xi640 Embedded Firmware

Basierend auf Beningos Zephyr Code-Review Methodik und projektspezifischen Regeln.

## Review-Ablauf

1. **Architektur-Check** — Passt der Code zur Schichtentrennung?
2. **DMA/Cache-Check** — Alignment, Flush/Invalidate korrekt?
3. **RTOS-Check** — Thread-Safety, ISR-Regeln eingehalten?
4. **USB-Check** — Deskriptoren, FIFO, Bandbreite korrekt?
5. **Zephyr-Patterns** — Devicetree, Kconfig, API-Nutzung idiomatisch?
6. **Coding-Standards** — Namenskonventionen, Fehlerbehandlung?

## Kritische Prüfpunkte (Showstopper)

### DMA / Cache (häufigste Fehlerquelle)
- [ ] Alle DMA-Buffer 32-Byte aligned (`__attribute__((aligned(32)))`)
- [ ] Cache Clean VOR jedem DMA TX (`sys_cache_data_flush_range`)
- [ ] Cache Invalidate NACH jedem DMA RX (`sys_cache_data_invd_range`)
- [ ] Keine DMA-Buffer in PSRAM für zeitkritische Pfade
- [ ] Buffer-Größen sind Vielfache von 32 Bytes

### RTOS Thread-Safety
- [ ] Keine `k_mutex_lock()` aus ISR-Kontext
- [ ] Keine `k_malloc()` im Hot Path (Streaming, ISR)
- [ ] `k_thread_suspend(tid)` statt `k_sleep(K_FOREVER)` für Ziel-Thread
- [ ] `k_thread_resume(tid)` statt `k_wakeup(tid)`
- [ ] `UX_WAIT_FOREVER` → `K_FOREVER` (nicht `K_MSEC(0xFFFFFFFF)`)
- [ ] Kein `printk()` / `LOG_*` im Hot Path

### USB Spezifisch
- [ ] `wMaxPacketSize = 0x1400` (nicht 0x0400)
- [ ] `dwMaxPayloadTransferSize = 24576` (nicht frame_size)
- [ ] Transfer-Blöcke 16-32 KB (nicht 512 B)
- [ ] SOF Even/Odd Frame Sync implementiert
- [ ] FIFO-Summe ≤ 4096 Words

### Zephyr-Patterns (nach Beningo)
- [ ] `device_is_ready()` IMMER nach `DEVICE_DT_GET()` prüfen
- [ ] `K_MSEC()` / `K_SECONDS()` statt rohe Zahlen für Timeouts
- [ ] typedef enum für Zustände (nicht nackte int / #define)
- [ ] Keine Magic Numbers — Defines mit Modul-Prefix
- [ ] Macro-Argumente geklammert: `#define MAX(a, b) ((a) > (b) ? (a) : (b))`

### Memory
- [ ] Keine dynamische Allokation nach Init-Phase
- [ ] Hot-Path-Buffer in AXISRAM (0x34xxxxxx)
- [ ] Netzwerk-Buffer dürfen in PSRAM (0x90xxxxxx)
- [ ] Stack-Größen angemessen (kein Overflow-Risiko)

## Stil-Prüfpunkte (Nicht-Blocker)

- [ ] Modulprefix bei allen Symbolen (`xi640_`, `ux_port_`)
- [ ] Deutsche Kommentare
- [ ] Return-Code Pattern statt void-Funktionen
- [ ] `static` für dateilokale Funktionen
- [ ] Assertions für Invarianten im Debug-Build
- [ ] Doxygen-Kommentare für öffentliche API

## Review-Output Format

```markdown
## Review: <Datei>

### Kritisch 🔴
- Zeile XX: [Problem] → [Lösung]

### Warnung 🟡
- Zeile XX: [Problem] → [Empfehlung]

### Verbesserung 🟢
- Zeile XX: [Vorschlag]

### Zusammenfassung
- X kritische Punkte, Y Warnungen, Z Verbesserungen
- Empfehlung: Merge / Nacharbeiten / Zurückweisen
```
