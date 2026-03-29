# DCMIPP-Pipeline Integration (P16 → P22) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** DCMIPP-Pipeline aus Projekt 16 in Projekt 22 integrieren — echte Thermaldaten statt Colorbar-Simulation über USB UVC streamen.

**Architecture:**
Zephyr UVC-Infrastruktur (sw-generator für Format-Advertisement, `video_enqueue/dequeue` für USB) bleibt bestehen. Ergänzt wird P16's DCMIPP HAL-Bypass-Pipeline (`dcmipp_capture.c`) + Security-Init. Der Timer-Simulator in `main.c` wird durch `dcmipp_capture_get_frame()` + `memcpy` ersetzt.

**Tech Stack:** Zephyr 4.3.99, STM32N6 HAL DCMIPP (`DCMIPP_BYPASS_HAL=1`), Zephyr USB `device_next`, STM32 RIF/RIFSC Security APIs

---

## Kritische Fakten (vor dem Start lesen!)

| Fakt | Konsequenz |
|------|-----------|
| DCMIPP Frame-Buffer liegen in AXISRAM1 (@ 0x34000000, per Linker-Section) | `dcmipp_buf.ld` MUSS in `CMakeLists.txt` eingebunden sein |
| `CONFIG_VIDEO_STM32_DCMIPP=y` ist nötig, damit `stm32n6xx_hal_dcmipp.c` kompiliert wird | Zephyr DCMIPP-Treiber muss aktiviert sein, auch im Bypass-Modus |
| `atto640d_video_stub.c` muss kompiliert werden | DCMIPP-Treiber prüft `device_is_ready(source_dev)` zur Laufzeit |
| Security-Init (RIMC/RISC/RISAF1 + IRQ-Fix) MUSS VOR `dcmipp_capture_init()` stehen | Sonst: Hard Fault beim ersten DCMIPP DMA-Zugriff |
| ATTO640D liefert 14-Bit ADC-Werte in `uint16_t` (nicht echtes YUYV!) | Option A: raw passthrough (Bild verfärbt aber Pipeline testbar), Option B: Konversion (Graustufen-YUYV) |
| P16's `dcmipp_capture.c` includiert `trace_udp.h` (nicht in P22) | Dependency vor dem Build entfernen |
| `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX` von 8 auf 2 reduzieren | AXISRAM-Heap ist nach dcmipp_buf.ld-Reservation knapp |
| `hsync-active = <0>` im DT-Overlay ist kritisch (BUG: 1 → DCMIPP liest nur Blanking-Zeilen) | Overlay 1:1 aus P16 übernehmen |

---

## Quelldateien Referenz

| Datei | Quelle | Ziel | Aktion |
|-------|--------|------|--------|
| `src/dcmipp_capture.c` | P16 | `app/src/dcmipp_capture.c` | Kopieren + trace_udp entfernen |
| `src/dcmipp_capture.h` | P16 | `app/src/dcmipp_capture.h` | Kopieren, unverändert |
| `src/atto640d_video_stub.c` | P16 | `app/src/atto640d_video_stub.c` | Kopieren, unverändert |
| `src/lynred_atto640d.c` | P16 | `app/src/lynred_atto640d.c` | ÜBERSCHREIBEN (P16 = aktueller) |
| `src/lynred_atto640d.h` | P16 | `app/src/lynred_atto640d.h` | ÜBERSCHREIBEN |
| `sections/dcmipp_buf.ld` | P16 | `app/sections/dcmipp_buf.ld` | Kopieren, unverändert |

P16-Basispfad: `D:/Zephyr_Workbench/project_16_test1_eth_minimal_DB_REVB/`

---

## Task 1: Quelldateien aus P16 nach P22 kopieren

**Files:**
- Create: `app/sections/dcmipp_buf.ld`
- Modify: `app/src/lynred_atto640d.c` (überschreiben)
- Modify: `app/src/lynred_atto640d.h` (überschreiben)
- Create: `app/src/dcmipp_capture.h`
- Create: `app/src/atto640d_video_stub.c`

**Schritt 1: Verzeichnis erstellen und Dateien kopieren**

```bash
P16=D:/Zephyr_Workbench/project_16_test1_eth_minimal_DB_REVB
P22=D:/Zephyr_Workbench/project_22/app

mkdir -p "$P22/sections"

cp "$P16/src/dcmipp_capture.h"        "$P22/src/dcmipp_capture.h"
cp "$P16/src/atto640d_video_stub.c"   "$P22/src/atto640d_video_stub.c"
cp "$P16/src/lynred_atto640d.c"       "$P22/src/lynred_atto640d.c"
cp "$P16/src/lynred_atto640d.h"       "$P22/src/lynred_atto640d.h"
cp "$P16/sections/dcmipp_buf.ld"      "$P22/sections/dcmipp_buf.ld"
```

**Schritt 2: dcmipp_capture.c mit angepasster trace_udp-Dependency kopieren**

`dcmipp_capture.c` enthält `#include "trace_udp.h"` und `TRACE0()` Aufrufe. P22 hat kein `trace_udp.c`. Die Datei muss angepasst werden:

Kopiere `$P16/src/dcmipp_capture.c` nach `$P22/src/dcmipp_capture.c`.

Dann ändere die ersten relevanten Zeilen: Ersetze `#include "trace_udp.h"` mit einem Stub:

```c
/* Trace-System nicht aktiv in P22 (kein CONFIG_TRACE_UDP) */
#ifdef CONFIG_TRACE_UDP
#include "trace_udp.h"
#else
#define TRACE0(x) ((void)0)
#endif
```

**Schritt 3: Verifikation**

```bash
ls app/src/dcmipp_capture.{c,h} app/src/atto640d_video_stub.c app/sections/dcmipp_buf.ld
# Erwartung: alle 5 Dateien vorhanden
head -5 app/src/dcmipp_capture.c
# Erwartung: KEIN "#include trace_udp.h" in Zeile 28, stattdessen #ifdef Guard
```

**Schritt 4: Commit**

```bash
cd D:/Zephyr_Workbench/project_22
git add app/src/dcmipp_capture.c app/src/dcmipp_capture.h
git add app/src/atto640d_video_stub.c
git add app/src/lynred_atto640d.c app/src/lynred_atto640d.h
git add app/sections/dcmipp_buf.ld
git commit -m "feat: DCMIPP-Quelldateien aus P16 übernommen (dcmipp_capture, atto640d_stub, dcmipp_buf.ld)"
```

---

## Task 2: CMakeLists.txt erweitern

**Files:**
- Modify: `app/CMakeLists.txt`

**Schritt 1: Neue Quelldateien und Linker-Section eintragen**

Aktueller Inhalt (`app/CMakeLists.txt`):
```cmake
cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_uvc_bulk)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE
    src/main.c
    src/lynred_atto640d.c
)
```

Neuer Inhalt:
```cmake
cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay"
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xi640_uvc_dcmipp)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
target_sources(app PRIVATE
    src/main.c
    src/lynred_atto640d.c
    src/atto640d_video_stub.c
    src/dcmipp_capture.c
)

# DCMIPP Frame-Buffer in AXISRAM1 @ 0x34000000 (vor Zephyr Heap)
zephyr_linker_sources(DATA_SECTIONS sections/dcmipp_buf.ld)
```

**Schritt 2: Commit**

```bash
git add app/CMakeLists.txt
git commit -m "feat: CMakeLists.txt — dcmipp_capture + atto640d_stub + dcmipp_buf.ld"
```

---

## Task 3: prj.conf erweitern

**Files:**
- Modify: `app/prj.conf`

**Schritt 1: DCMIPP-Konfiguration hinzufügen**

Folgende Zeilen ans Ende von `app/prj.conf` anfügen:

```ini
# ── DCMIPP Parallel-Interface (14-Bit Pipe0 Dump Mode) ──────────────────────
CONFIG_VIDEO_STM32_DCMIPP=y
# RGB565 = FourCC "RGBP", bpp=16 → pitch=1280 korrekt (nicht SBGGR8 mit pitch=640!)
CONFIG_VIDEO_STM32_DCMIPP_SENSOR_PIXEL_FORMAT="RGBP"
CONFIG_VIDEO_STM32_DCMIPP_SENSOR_WIDTH=640
CONFIG_VIDEO_STM32_DCMIPP_SENSOR_HEIGHT=480
CONFIG_VIDEO_BUFFER_POOL_HEAP_SIZE=1024

# Kritisch: ohne dynamic interrupts zeigt DCMIPP IRQ48 auf Flash (Hard Fault!)
CONFIG_DYNAMIC_INTERRUPTS=y

# Speicher: DCMIPP beansprucht 1.2 MB AXISRAM1 → Heap ist knapp → max 2 UVC-Buffer
CONFIG_VIDEO_BUFFER_POOL_NUM_MAX=2

# MEM_ATTR_HEAP führt zu Assertion-Fehler bei Boot (bekanntes P16-Problem)
CONFIG_MEM_ATTR_HEAP=n

# I-Cache für bessere Performance (D-Cache ist bereits aktiv)
CONFIG_ICACHE=y
```

**Hinweis zu `CONFIG_VIDEO_BUFFER_POOL_NUM_MAX`:** Von 8 auf 2 reduziert. Begründung: `dcmipp_buf.ld` reserviert 1.2 MB am Anfang von AXISRAM1 für DCMIPP Frame-Buffer. Der verbleibende Heap reicht nicht für 8 × 614 KB UVC-Buffer. 2 Buffer (= 1.2 MB) passen knapp, da AXISRAM3 (512 KB, aktiviert) zusätzlich zum Heap beiträgt.

**Schritt 2: Verifikation — Keine Konflikte**

```bash
grep "VIDEO_BUFFER_POOL_NUM_MAX" app/prj.conf
# Erwartung: nur EINE Zeile, Wert = 2
grep "VIDEO_STM32_DCMIPP" app/prj.conf
# Erwartung: 4 Zeilen mit DCMIPP-Config
```

**Schritt 3: Commit**

```bash
git add app/prj.conf
git commit -m "feat: prj.conf — CONFIG_VIDEO_STM32_DCMIPP + DYNAMIC_INTERRUPTS + Buffer-Tuning"
```

---

## Task 4: Boards-Overlay erweitern — DCMIPP + ATTO640D Video-Node

**Files:**
- Modify: `app/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay`

**Schritt 1: Was bereits im P22-Overlay vorhanden ist (NICHT doppelt eintragen)**
- `&usart10` Console-Node ✓
- AXISRAM3-6 `status = "okay"` ✓
- PSRAM (`&aps256xxn_obr`) ✓
- ATTO640D GPIO-Node (`atto640d_ctrl`) ✓
- `&i2c1` Sensor-I2C ✓
- `&timers3` + `atto640d_mc_pwm` ✓

**Schritt 2: Folgende Blöcke ANS ENDE des bestehenden Overlays anfügen**

```dts
/*
 * ── Lynred ATTO640D-04 Video-Source Node (für DCMIPP-Treiber) ─────────────
 * Der DCMIPP-Treiber (video_stm32_dcmipp.c) benötigt ein Zephyr-Device am
 * Source-Endpoint. Dieser Node registriert den atto640d_video_stub.
 * Echte Hardware-Ansteuerung läuft über atto640d_init() in lynred_atto640d.c.
 */
/ {
    atto640d: atto640d {
        compatible = "lynred,atto640d-04";
        status = "okay";

        port {
            atto640d_ep_out: endpoint {
                remote-endpoint-label = "dcmipp_ep_in";
            };
        };
    };
};

/*
 * ── DCMIPP: 14-Bit Parallel Interface ─────────────────────────────────────
 *
 * Pin-Mapping (STM32N6570-DK Schaltplan):
 *   Data[0..13]: PD7 PE6 PC5 PE10 PE8 PE4 PB6 PB7 PE1 PD4 PB14 PD8 PD12 PD13
 *   PIXCLK: PD5 (PSYNC via CPLD), HSYNC: PC0, VSYNC: PB8
 *
 * Polaritäten (kritisch — aus P16 bestätigt):
 *   hsync-active = 0: HSYNC active LOW  ← BUGFIX (1 → DCMIPP las nur Blanking!)
 *   vsync-active = 0: VSYNC active LOW
 *   pclk-sample  = 0: Sample auf FALLENDER PSYNC-Flanke (PCKPOL=0)
 *
 * data-lanes: Dummy für STM32_DCMIPP_HAS_CSI compile-time Prüfung.
 * STM32N6 hat CSI-HW → Treiber liest data_lanes auch im Parallel-Modus.
 * Zur Laufzeit ignoriert (bus-type=PARALLEL).
 */
&dcmipp {
    status = "okay";
    pinctrl-0 = <
        &dcmipp_pixclk_pd5
        &dcmipp_hsync_pc0
        &dcmipp_vsync_pb8
        &dcmipp_d0_pd7
        &dcmipp_d1_pe6
        &dcmipp_d2_pc5
        &dcmipp_d3_pe10
        &dcmipp_d4_pe8
        &dcmipp_d5_pe4
        &dcmipp_d6_pb6
        &dcmipp_d7_pb7
        &dcmipp_d8_pe1
        &dcmipp_d9_pd4
        &dcmipp_d10_pb14
        &dcmipp_d11_pd8
        &dcmipp_d12_pd12
        &dcmipp_d13_pd13
    >;
    pinctrl-names = "default";

    port {
        dcmipp_ep_in: endpoint {
            remote-endpoint-label = "atto640d_ep_out";
            bus-type = <VIDEO_BUS_TYPE_PARALLEL>;
            bus-width = <14>;
            hsync-active = <0>;
            vsync-active = <0>;
            pclk-sample  = <0>;
            data-lanes = <1 2>;
        };
    };
};
```

**Schritt 3: Verifikation**

```bash
grep -c "dcmipp_ep_in\|atto640d_ep_out\|lynred,atto640d-04" \
  app/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay
# Erwartung: 3 (jedes Keyword einmal vorhanden)
```

**Schritt 4: Commit**

```bash
git add app/boards/stm32n6570_dk_stm32n657xx_fsbl.overlay
git commit -m "feat: Overlay — DCMIPP Parallel-Interface + ATTO640D Video-Source-Node"
```

---

## Task 5: app.overlay — sw-generator bleibt für Format-Advertisement

**Files:**
- Read: `app/app.overlay` (zur Kontrolle — keine Änderung nötig)

**Schritt 1: Prüfen, dass app.overlay unverändert bleibt**

```bash
cat app/app.overlay
```

Erwarteter Inhalt (KEINE Änderung nötig):
```dts
/ {
    chosen {
        zephyr,camera = &sw_generator;
    };

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

**Begründung:** Der `sw_generator` bleibt als `zephyr,camera` erhalten, weil er für die UVC Format-Advertisement (`video_get_caps()` + `uvc_add_format()`) genutzt wird. Er liefert KEINE echten Frames — das übernimmt DCMIPP. Der sw-generator gibt Formate wie YUYV 640×480 bekannt, die Windows korrekt verarbeiten kann.

**Keine Aktion nötig — kein Commit.**

---

## Task 6: main.c — Security-Init + DCMIPP-Streaming-Loop

**Files:**
- Modify: `app/src/main.c`

Dies ist die umfangreichste Änderung. Der bestehende main.c wird in drei Bereichen modifiziert:

### 6a: Includes erweitern

Am Anfang von `main.c`, nach den bestehenden Includes:

```c
/* NEU: DCMIPP Pipeline */
#include <soc.h>              /* STM32N6 HAL: RIF, RCC, NVIC */
#include "dcmipp_capture.h"
```

Die Zeile `#include "lynred_atto640d.h"` ist bereits vorhanden — NICHT doppelt eintragen.

### 6b: Timer-Simulation ENTFERNEN

Diese Blöcke aus main.c entfernen (sie werden durch DCMIPP ersetzt):

```c
/* ENTFERNEN: */
#define DCMIPP_SIM_PERIOD_MS  25U

static struct k_sem frame_ready_sem;
static struct k_timer dcmipp_sim_timer;

static void dcmipp_sim_timer_handler(struct k_timer *timer)
{
    k_sem_give(&frame_ready_sem);
}
```

### 6c: main() — Security-Init + DCMIPP-Init einfügen

In `main()`, NACH dem existierenden `atto640d_init()` Block und VOR `device_is_ready(video_dev)` diesen Block einfügen:

```c
    /* ── Security-Init: DCMIPP RIF-Zugriff freigeben ──────────────────────
     * MUSS vor dcmipp_capture_init() stehen!
     * Ohne diese Konfiguration: DCMIPP IRQ zeigt auf Flash → Hard Fault.
     * Identisch mit P16 main.c (bewährt, getestet). */

    /* APB5 Bus + DCMIPP Peripheral Clock */
    RCC->BUSENR  |= RCC_BUSENR_APB5EN;
    RCC->APB5ENR |= RCC_APB5ENR_DCMIPPEN;
    LOG_INF("DCMIPP: APB5EN + DCMIPPEN gesetzt (0x%08x)", RCC->APB5ENR);

    /* RIFSC: DCMIPP DMA-Master + Peripheral-Slave → CID1 Secure+Priv */
    __HAL_RCC_RIFSC_CLK_ENABLE();
    {
        RIMC_MasterConfig_t rimc = { 0 };
        rimc.MasterCID = RIF_CID_1;
        rimc.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
        HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &rimc);
        HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP,
                                              RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    }
    LOG_INF("DCMIPP RIFSC: CID1 Secure+Priv gesetzt");

    /* DCMIPP IRQ48 Fix: ITNS[1] bit16 auf Secure zwingen + SCB_NS->VTOR korrigieren.
     * Bug: ITNS[1] bit16=1 (NonSecure) → Vektor liest aus SCB_NS->VTOR → Flash
     * (0x18005abc) → Hard Fault. Fix A: bit löschen. Fix B: NS-VTOR korrigieren. */
    {
        uint32_t vtor_s = SCB->VTOR;
        if (NVIC->ITNS[1] & (1U << 16)) {
            LOG_WRN("ITNS bit16 gesetzt — korrigiere DCMIPP auf Secure");
            irq_disable(48);
            NVIC->ITNS[1] &= ~(1U << 16);
            irq_enable(48);
        }
        if (SCB_NS->VTOR != vtor_s) {
            LOG_WRN("SCB_NS->VTOR falsch (0x%08x), korrigiere", SCB_NS->VTOR);
            SCB_NS->VTOR = vtor_s;
        }
        LOG_INF("DCMIPP IRQ: ITNS[1]=0x%08x VTOR_NS=0x%08x",
                NVIC->ITNS[1], SCB_NS->VTOR);
    }

    /* RISAF1: DCMIPP DMA → PSRAM (0x90000000) write access für CID1 */
    {
        RISAF_BaseRegionConfig_t risaf_cfg = { 0 };
        risaf_cfg.Filtering      = RISAF_FILTER_ENABLE;
        risaf_cfg.Secure         = RIF_ATTRIBUTE_SEC;
        risaf_cfg.PrivWhitelist  = RIF_CID_1;
        risaf_cfg.ReadWhitelist  = RIF_CID_1;
        risaf_cfg.WriteWhitelist = RIF_CID_1;
        risaf_cfg.StartAddress   = 0x00000000U;
        risaf_cfg.EndAddress     = 0x01FFFFFFU;
        HAL_RIF_RISAF_ConfigBaseRegion(RISAF1, RISAF_REGION_1, &risaf_cfg);
        LOG_INF("RISAF1: CID1 PSRAM 0x90000000..0x91FFFFFF konfiguriert");
    }

    /* DCMIPP Parallel-Interface Init (HAL Bypass Modus) */
    LOG_INF("=== DCMIPP Capture Init ===");
    ret = dcmipp_capture_init();
    if (ret != 0) {
        LOG_ERR("dcmipp_capture_init fehlgeschlagen: %d", ret);
        /* Weiterlaufen mit leerem Bild ist nicht sinnvoll */
        return ret;
    }
    LOG_INF("DCMIPP: HAL-Bypass Streaming aktiv");
```

### 6d: Streaming-Loop ersetzen

Den bestehenden Loop-Body ersetzen. Aktuell:

```c
/* ---- DCMIPP-Simulation initialisieren ---- */
k_sem_init(&frame_ready_sem, 0, 1);
k_timer_init(&dcmipp_sim_timer, dcmipp_sim_timer_handler, NULL);
...
k_timer_start(&dcmipp_sim_timer, ...);
...
while (true) {
    k_sem_take(&frame_ready_sem, K_FOREVER);
    ret = video_dequeue(uvc_dev, &done_buf, K_NO_WAIT);
    ...
    fill_frame_counter(done_buf, frame_nr++);
    ...
    video_enqueue(uvc_dev, done_buf);
}
```

Ersetzen mit:

```c
    /* ── DCMIPP Erster Frame — Sensor + Pipeline prüfen ──────────────────── */
    {
        uint8_t *test_data;
        uint32_t test_size;
        LOG_INF("Warte auf ersten DCMIPP-Frame (max 2 s)...");
        ret = dcmipp_capture_get_frame(&test_data, &test_size, 2000);
        if (ret != 0) {
            LOG_ERR("Kein DCMIPP-Frame nach 2 s (ret=%d) — Sensor-Problem?", ret);
            /* Weiterlaufen: Loop wird mit Timeout-Skipping arbeiten */
        } else {
            /* Statistik: erste 5 Pixel ausgeben (14-Bit Maske) */
            uint16_t *px = (uint16_t *)test_data;
            LOG_INF("DCMIPP OK: [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u (14-Bit)",
                    px[0] & 0x3FFF, px[1] & 0x3FFF, px[2] & 0x3FFF,
                    px[3] & 0x3FFF, px[4] & 0x3FFF);
        }
    }

    bench_reset(frame_size);

    /* ── DCMIPP → UVC Streaming-Loop ─────────────────────────────────────── */
    while (true) {
        /* 1. Warte auf DCMIPP-Frame (blockiert bis DMA fertig, max 100 ms) */
        uint8_t *dcmipp_data;
        uint32_t dcmipp_size;
        ret = dcmipp_capture_get_frame(&dcmipp_data, &dcmipp_size, 100);
        if (ret == -ETIMEDOUT) {
            LOG_WRN("DCMIPP Timeout — Sensor antwortet nicht");
            k_sleep(K_MSEC(1));
            continue;
        }
        if (ret != 0) {
            LOG_ERR("DCMIPP Fehler: %d", ret);
            break;
        }

        /* 2. UVC-Buffer holen (K_NO_WAIT: Frame droppen wenn USB zu langsam) */
        ret = video_dequeue(uvc_dev, &done_buf, K_NO_WAIT);
        if (ret != 0) {
            /* Kein Buffer frei: Frame wurde gedroppt.
             * DCMIPP hat bereits den nächsten Frame angefordert (in get_frame). */
            LOG_DBG("Frame gedroppt — kein UVC-Buffer frei (USB-Backpressure)");
            continue;
        }

        bench_update(frame_size);

        /* 3. DCMIPP-Daten → UVC-Buffer kopieren.
         *
         * Option A (aktiv): Raw passthrough — 14-Bit uint16_t direkt als YUYV.
         *   Windows zeigt verfärbtes Bild, aber Pipeline-Geschwindigkeit messbar.
         *   dcmipp_size == frame_size == 614400 Bytes (640×480×2).
         *
         * Option B (zum Aktivieren): 14-Bit → Graustufen-YUYV konvertieren:
         *   uint16_t *src = (uint16_t *)dcmipp_data;
         *   uint8_t  *dst = done_buf->buffer;
         *   for (int i = 0; i < 640*480; i += 2) {
         *       dst[i*2+0] = (src[i]   >> 6) & 0xFF;  // Y0
         *       dst[i*2+1] = 128;                       // U (neutral)
         *       dst[i*2+2] = (src[i+1] >> 6) & 0xFF;  // Y1
         *       dst[i*2+3] = 128;                       // V (neutral)
         *   }
         */
        memcpy(done_buf->buffer, dcmipp_data, MIN(dcmipp_size, frame_size));
        done_buf->bytesused = frame_size;

        /* 4. Buffer zurück in USB-Queue */
        done_buf->type = VIDEO_BUF_TYPE_INPUT;
        ret = video_enqueue(uvc_dev, done_buf);
        if (ret != 0) {
            LOG_ERR("video_enqueue fehlgeschlagen: %d", ret);
            break;
        }
    }

    return 0;
```

**Schritt 5: fill_frame_counter() entfernen**

Die Funktion `fill_frame_counter()` wird nicht mehr benötigt und kann entfernt werden (oder kommentiert lassen).

**Schritt 6: bench_reset() Signatur prüfen**

`bench_reset(frame_size)` — der Aufruf bleibt, aber `bench_reset` braucht keinen Parameter mehr wenn kein `DCMIPP_SIM_PERIOD_MS` mehr geloggt wird. Aktuell loggt sie `DCMIPP_SIM_PERIOD_MS` — nach Entfernung des Defines muss diese Zeile angepasst werden:

In `bench_reset()`:
```c
/* Vorher: */
LOG_INF("Benchmark gestartet, Frame-Groesse: %u Bytes, Periode: %u ms",
    frame_size, DCMIPP_SIM_PERIOD_MS);

/* Nachher: */
LOG_INF("Benchmark gestartet, Frame-Groesse: %u Bytes (DCMIPP-Quelle)",
    frame_size);
```

**Schritt 7: Commit**

```bash
git add app/src/main.c
git commit -m "feat: main.c — Security-Init + DCMIPP HAL-Bypass ersetzt Timer-Simulation"
```

---

## Task 7: Build — west build --pristine

**Schritt 1: Pristine Build (PFLICHT nach Overlay-Änderungen)**

```bash
cd D:/Zephyr_Workbench/project_22/app
west build --pristine -b stm32n6570_dk/stm32n657xx/fsbl -d ../build/primary .
```

**Erwartete Warnungen (harmlos):**
- `WARNING: ...dcmipp_capture.c:XX: implicit declaration` → falls `<zephyr/video/video.h>` fehlt
- Fix: Entferne `#include <zephyr/video/video.h>` aus dcmipp_capture.c (bereits in `<zephyr/drivers/video.h>`)

**Erwartete Build-Fehler und Fixes:**

| Fehler | Fix |
|--------|-----|
| `error: 'TRACE0' undeclared` | `#ifdef CONFIG_TRACE_UDP` Guard fehlt → Task 1 Schritt 2 wiederholen |
| `error: 'pipe_dump' undeclared` | `DT_NODELABEL(pipe_dump)` Compile-Fehler — `#define DCMIPP_NODE` nur in `#if !DCMIPP_BYPASS_HAL` wrappen |
| `error: unknown type name 'DCMIPP_HandleTypeDef'` | `CONFIG_VIDEO_STM32_DCMIPP=y` fehlt in prj.conf |
| `error: 'CONFIG_APP_VIDEO_MAX_RESOLUTIONS' undeclared` | In `prj.conf` hinzufügen: `CONFIG_APP_VIDEO_MAX_RESOLUTIONS=5` |
| `error: HAL_RIF_... undeclared` | `#include <soc.h>` fehlt in main.c |
| Linker: Multiple definition of `lynred_atto640d_init` | lynred_atto640d.c doppelt im build — in CMakeLists.txt nur EINMAL eintragen |

**Schritt 2: Build-Ausgabe prüfen**

```bash
# Erwartung: "Memory region ... AXISRAM1 used: ..."
# Prüfen ob dcmipp_frames section platziert wurde:
west build -d ../build/primary 2>&1 | grep -i "dcmipp\|axisram"
```

**Schritt 3: Flash**

```bash
west flash -d ../build/primary
```

---

## Task 8: Verifikation und Debug

### 8a: UART-Log beim Start prüfen

Erwartete Log-Sequenz (USART10, 115200 Baud):

```
[00:00:00.500] ATTO640D: Sequencer aktiv, Free-run @ 33 MHz
[00:00:00.501] DCMIPP: APB5EN + DCMIPPEN gesetzt (0x00000024)
[00:00:00.501] DCMIPP RIFSC: CID1 Secure+Priv gesetzt
[00:00:00.501] DCMIPP IRQ: ITNS[1]=0x... VTOR_NS=0x34180400
[00:00:00.501] RISAF1: CID1 PSRAM 0x90000000..0x91FFFFFF konfiguriert
[00:00:00.501] BYPASS HW-Reset: RCC APB5 FORCE+RELEASE — PRCR=0x0 ENABLE=0
[00:00:00.510] BYPASS PRCR nach PARALLEL_SetConfig: 0x004D4060 (FORMAT=0x4D EDM=1)
[00:00:00.510] BYPASS Init OK — buf_a=0x34000000 buf_b=0x34096000
[00:00:00.510] DCMIPP: HAL-Bypass Streaming aktiv
[00:00:00.530] DCMIPP OK: [0]=4096 [1]=4097 [2]=4095 ... (sinnvolle 14-Bit Werte)
[00:00:00.530] Warte auf Host Format-Auswahl...
```

**Wenn stattdessen erscheint:**
- `BYPASS PRCR nach PARALLEL_SetConfig: 0x00000000` → DCMIPP Clock fehlt (APB5ENR-Fix)
- `DCMIPP Timeout` → Sensor-Problem (ATTO640D nicht initialisiert, MC nicht läuft)
- Hard Fault → Security-Init fehlt oder falsche Reihenfolge

### 8b: USB-Enumeration prüfen

1. **Windows Gerätemanager:** USB-Videogerät erscheint → ✅
2. **AMCap oder Windows Kamera-App öffnen** → Gerät wählen → Bild erscheint
3. **Log prüfen:**
   ```
   Host gewählt: YUYV 640x480 @ 30/1
   USB Speed: 1 (1=HS), MPS: 512
   Bench: 30 Frames | 25 FPS | 147456 kbit/s
   ```

### 8c: Bildqualität bewerten

| Beobachtung | Bedeutung |
|-------------|-----------|
| Bild sichtbar aber bunte Artefakte | Option A (raw passthrough) funktioniert → Option B aktivieren |
| Bild schwarz-weiß Graustufen | Option B aktiviert und korrekt |
| Bild komplett schwarz | DCMIPP-Daten kommen nicht durch → `dcmipp_data` prüfen |
| Bild bleibt eingefroren (1 Frame) | `video_dequeue()` gibt nie `done_buf` zurück → Buffer-Problem |
| Bild "sägezahn"-artig verschoben | `pclk-sample = <1>` statt `<0>` versuchen (PCKPOL-Polarität) |
| Bild "gestaucht" (nur halbe Höhe) | `VIDEO_PIX_FMT_RGB565` in Stub falsch → pitch prüfen |

### 8d: Option B aktivieren (Graustufen-YUYV)

In `main.c` den `memcpy`-Block durch die YUYV-Konversion ersetzen:

```c
/* 14-Bit → Graustufen YUYV */
const uint16_t *src = (const uint16_t *)dcmipp_data;
uint8_t *dst = done_buf->buffer;
const int pixels = DCMIPP_FRAME_WIDTH * DCMIPP_FRAME_HEIGHT;

for (int i = 0; i < pixels; i += 2) {
    dst[i*2+0] = (src[i]   >> 6) & 0xFF;  /* Y0: 14-Bit → 8-Bit */
    dst[i*2+1] = 128;                       /* U (neutral = grau) */
    dst[i*2+2] = (src[i+1] >> 6) & 0xFF;  /* Y1 */
    dst[i*2+3] = 128;                       /* V (neutral = grau) */
}
done_buf->bytesused = frame_size;
```

**Hinweis:** Die Konversion kostet ~2-3 ms für 640×480 Pixel. Mit DCMIPP-Frame-Zeit ~20 ms bleibt genug Budget.

---

## Häufige Fallstricke (aus P16-Erfahrung)

1. **`west build` ohne `--pristine` nach Overlay-Änderungen** → Zephyr cached DTS-Dateien → Build scheinbar ok aber falsches DT → `--pristine` PFLICHT
2. **`VIDEO_BUFFER_POOL_NUM_MAX` zu hoch** → ENOMEM beim Buffer-Alloc → Build läuft, Boot crasht
3. **`hsync-active = <1>`** → DCMIPP liest Blanking-Zeilen statt Pixel → leeres Bild oder Artefakte
4. **Security-Init nach dcmipp_capture_init()** → Hard Fault beim ersten DMA-Transfer
5. **Kein `CONFIG_DYNAMIC_INTERRUPTS=y`** → DCMIPP IRQ48 zeigt auf Flash → Hard Fault

---

## Definition of Done

- [ ] `west build --pristine` ohne Fehler ✅
- [ ] UART-Log zeigt "DCMIPP OK: [0]=...[1]=..." (sinnvolle Pixelwerte) ✅
- [ ] Windows Gerätemanager: "USB-Videogerät" ✅
- [ ] AMCap / Windows Kamera: Bild sichtbar (Option A: Farbartefakte OK für Test) ✅
- [ ] Bench-Log: FPS > 15 bei 640×480 ✅
- [ ] Kein Reconnect-Crash nach 3× USB-Trennen/Verbinden ✅
- [ ] Option B (Graustufen-YUYV): Thermalbild erkennbar ✅
- [ ] Git commit + Push ✅

---

## Nächste Schritte nach Abschluss

1. **Phase ISO-1:** `usbd_uvc_iso.c` aktivieren (3×1024 ISO statt Bulk → ~24 MB/s)
2. **Ethernet parallel:** `thermal_stream.c` aus P16 integrieren (UDP Port 9999)
3. **Auto-Level:** `atto640d_auto_level()` aktivieren (DAC_GSK iterativ anpassen)
4. **NUC-Korrektur:** `Corrected = Dark[i] - Raw[i]` (invertiertes ADC-Signal)
