---
name: tdd-embedded
description: Test-Driven Development für Embedded Firmware. Aktiviere wenn Tests geschrieben, Testpläne erstellt oder Code nach TDD-Methodik entwickelt werden soll. Besonders für hostbasierte Tests ohne Hardware.
---

# TDD für Embedded Firmware (Xi640)

## Grundprinzip

Red → Green → Refactor. Kein Produktivcode ohne vorherigen fehlschlagenden Test.

## Zwei Test-Welten

```
┌─────────────────────────────┐
│  Host-testbar (ohne HW)     │  ← TDD hier voll anwenden
│                             │
│  - ux_port.c Logik          │  Framework: Zephyr ztest
│  - UVC Deskriptoren         │  Board: native_posix / native_sim
│  - Probe/Commit Werte       │  Laufzeit: Sekunden
│  - Frame Dispatcher Logik   │  Automatisierbar: Ja (CI)
│  - Timeout-Konvertierungen  │
│  - State Machines           │
│  - Payload Framing          │
└──────────────┬──────────────┘
               │ HAL-Interface (Abstraktionsgrenze)
┌──────────────┴──────────────┐
│  Hardware-abhängig          │  ← hw-feedback-loop Skill nutzen
│                             │
│  - OTG HS Register          │  Validierung: Manuell auf Target
│  - DMA Transfers            │  Feedback: Ergebnis an Claude melden
│  - DCMIPP Pipeline          │  Iterationszeit: Minuten
│  - PHY Init                 │
│  - Cache-Operationen        │
└─────────────────────────────┘
```

## Test-Framework

Zephyr `ztest` für Host-Tests:

```c
#include <zephyr/ztest.h>

ZTEST_SUITE(ux_port_timeout, NULL, NULL, NULL, NULL, NULL);

ZTEST(ux_port_timeout, test_wait_forever_mapping)
{
    k_timeout_t result = ux_to_zephyr_timeout(UX_WAIT_FOREVER);
    zassert_true(K_TIMEOUT_EQ(result, K_FOREVER),
                 "UX_WAIT_FOREVER muss auf K_FOREVER mappen");
}

ZTEST(ux_port_timeout, test_zero_ticks)
{
    k_timeout_t result = ux_to_zephyr_timeout(0);
    zassert_true(K_TIMEOUT_EQ(result, K_NO_WAIT),
                 "0 Ticks muss auf K_NO_WAIT mappen");
}

ZTEST(ux_port_timeout, test_normal_ticks)
{
    k_timeout_t result = ux_to_zephyr_timeout(100);
    zassert_equal(result.ticks, 100,
                  "100 Ticks muss 100 Ticks bleiben");
}
```

## Build-Kommandos

```bash
# Host-Tests bauen und ausführen (schnell, kein Flash nötig)
west build -b native_posix app/tests -d build/test
./build/test/zephyr/zephyr.exe

# Target-Build prüfen (darf nicht brechen)
west build -d build/primary

# Beides muss grün sein bevor commit!
```

## Test-Kategorien nach Priorität

### 1. Port-Layer Tests (HOCH — Phase 3)
- Timeout-Mapping (UX_WAIT_FOREVER, 0, Normalwerte, Grenzwerte)
- Thread-Lifecycle (Create → Suspend → Resume → Delete)
- Semaphore-Semantik (Put/Get, Timeout, Count)
- Mutex-Semantik (Lock/Unlock, Rekursion)
- Memory-Allokation (Alignment-Prüfung, kein Leak)

### 2. UVC Deskriptor Tests (HOCH — Phase 5)
- wMaxPacketSize korrekt kodiert (0x1400)
- Probe/Commit Werte konsistent
- dwMaxPayloadTransferSize = 24576 (nicht frame_size)
- frameInterval = 333333 (30 FPS in 100ns Einheiten)
- Deskriptor-Gesamtlänge korrekt berechnet

### 3. Payload Framing Tests (MITTEL — Phase 5)
- UVC Header korrekt (2 Bytes, frame_id toggle)
- EOF-Bit im letzten Paket gesetzt
- Payload ≤ 24574 Bytes pro Transfer (24576 - 2 Header)
- Frame-ID wechselt zwischen Frames

### 4. State Machine Tests (MITTEL — Phase 5-6)
- Stream Start/Stop Übergänge
- Fehlerbehandlung bei DMA-Fehlern
- Reconnect-Verhalten

## Namenskonvention für Tests

```
test_<modul>_<funktion>_<szenario>

Beispiele:
test_ux_port_timeout_wait_forever
test_ux_port_thread_suspend_target_not_caller
test_uvc_desc_wmaxpacketsize_high_bandwidth
test_uvc_payload_eof_bit_last_packet
```

## Akzeptanzkriterien

- Alle Host-Tests MÜSSEN ohne Hardware laufen (`native_posix`)
- Keine `#ifdef` im Testcode — Tests müssen deterministisch sein
- Jeder Test prüft genau EINE Sache
- Test-Namen beschreiben das erwartete Verhalten
- Target-Build darf nie brechen wenn Host-Tests hinzukommen
