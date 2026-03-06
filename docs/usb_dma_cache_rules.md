# USB DMA Cache Rules
## STM32N6 Cortex-M55 — Xi640 Project 22

### Grundregel

Cortex-M55 hat D-Cache (32-Byte Cachelines).
DMA umgeht den Cache. Ohne Maintenance: Datenkorrumption.

### TX (USB ISO IN — Daten senden)

```c
// VOR dem DMA Transfer: Cache in RAM schreiben
ux_cache_clean(tx_buf, tx_len);
// Dann DMA starten
```

### RX (Control Transfers — Daten empfangen)

```c
// NACH dem DMA Transfer: Cache-Zeilen verwerfen
ux_cache_invalidate(rx_buf, rx_len);
// Dann CPU liest frische Daten aus RAM
```

### Alignment

- DMA Buffer MUSS 32-Byte aligned sein
- `K_HEAP_DEFINE` oder `__aligned(32)` verwenden
- `ux_cache_clean/invalidate` runden automatisch ab/auf

### PSRAM Besonderheit

- PSRAM Buffers haben zusätzliche XSPI-Latenz
- ISO TX Buffer NICHT in PSRAM
- Für ISO TX nur AXI SRAM verwenden

### Verboten

- Cache-Maintenance aus ISR ohne Alignment-Prüfung
- ISO TX Buffer in PSRAM
- Mutex aus ISR (nur Semaphore ISR-safe)
