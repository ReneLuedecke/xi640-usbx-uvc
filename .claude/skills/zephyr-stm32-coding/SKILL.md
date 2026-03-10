---
name: zephyr-stm32-coding
description: Zephyr RTOS und STM32 Coding-Standards für Embedded-Firmware. Aktiviere bei jedem C-Code der Zephyr-APIs, STM32-HAL, DMA, Cache, USB OTG oder RTOS-Primitiven verwendet.
---

# Zephyr / STM32 Embedded Coding Standards

## Sprache

Alle Kommentare, Commit-Messages und Dokumentation auf **Deutsch**.

## Namenskonventionen

```c
// Module-Prefix für alle Symbole
xi640_uvc_*       // UVC-Modul
xi640_dcd_*       // DCD-Modul
xi640_dcmipp_*    // DCMIPP-Modul
ux_port_*         // USBX Port-Layer

// Typen: snake_case mit _t Suffix
typedef struct xi640_frame_desc {
    uint8_t *data;
    uint32_t size;
    uint32_t timestamp_ms;
} xi640_frame_desc_t;

// Defines: GROSSBUCHSTABEN mit Modul-Prefix
#define XI640_UVC_FRAME_WIDTH     640U
#define XI640_UVC_FRAME_HEIGHT    480U
#define XI640_ISO_MAX_PAYLOAD     24576U

// Enums: Typ snake_case, Werte GROSSBUCHSTABEN
typedef enum {
    XI640_STATUS_OK = 0,
    XI640_STATUS_ERR_DMA,
    XI640_STATUS_ERR_TIMEOUT,
} xi640_status_t;

// Funktionen: snake_case, immer Modul-Prefix
xi640_status_t xi640_uvc_stream_start(void);
```

## DMA / Cache Regeln (Cortex-M55)

**Jeder DMA-Buffer MUSS diese Regeln einhalten:**

```c
// 1. Alignment: 32 Byte (Cache-Line)
__attribute__((aligned(32))) static uint8_t iso_buf[BUF_SIZE];

// 2. VOR DMA TX: Cache Clean (CPU → Memory)
sys_cache_data_flush_range(buf, size);     // Zephyr-Wrapper bevorzugen
SCB_CleanDCache_by_Addr((uint32_t *)buf, size);  // CMSIS Alternative

// 3. NACH DMA RX: Cache Invalidate (Memory → CPU)
sys_cache_data_invd_range(buf, size);      // Zephyr-Wrapper bevorzugen
SCB_InvalidateDCache_by_Addr((uint32_t *)buf, size);

// 4. Buffer-Größen: Vielfache von 32 Bytes
#define BUF_SIZE  ROUND_UP(raw_size, 32)
```

**Verboten:**
- DMA-Buffer in PSRAM für zeitkritische Pfade (ISO, DCMIPP)
- Unaligned Buffer für DMA
- Cache-Operationen vergessen → "läuft 10 Sekunden, dann tot"

## RTOS-Patterns

```c
// Statische Allokation bevorzugen
K_THREAD_STACK_DEFINE(uvc_stack, 4096);
static struct k_thread uvc_thread;

// Keine Heap-Allokation im Hot Path
// FALSCH:
void *buf = k_malloc(size);  // ← Verboten im Streaming-Pfad

// RICHTIG:
static uint8_t buf[SIZE] __attribute__((aligned(32)));

// ISR-Regeln:
// ERLAUBT in ISR: k_sem_give(), k_fifo_put()
// VERBOTEN in ISR: k_mutex_lock(), k_malloc(), printk() im Hot Path

// Zephyr Devicetree Zugriff (nicht direkt Register):
const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpioa));
if (!device_is_ready(gpio)) {
    LOG_ERR("GPIO nicht bereit");
    return XI640_STATUS_ERR_INIT;
}
```

## USB OTG HS Patterns

```c
// FIFO-Register immer mit Kommentar
USB_OTG_HS->GRXFSIZ    = 1024;                    // RX: 1024 Words
USB_OTG_HS->DIEPTXF0   = (512U << 16) | 1024U;   // EP0 TX: 512 Words @ Offset 1024
USB_OTG_HS->DIEPTXF[0] = (2048U << 16) | 1536U;  // EP1 ISO TX: 2048 Words @ Offset 1536

// wMaxPacketSize für High-Bandwidth ISO
// Bits 10:0 = 1024, Bits 12:11 = 10b (3 Transactions)
#define XI640_ISO_WMAXPACKETSIZE  0x1400U

// Transfer-Größe: 16-32 KB Blöcke, NICHT 512 B
#define XI640_ISO_TRANSFER_SIZE   16384U

// UVC Probe/Commit — diese Werte sind KRITISCH:
// dwMaxPayloadTransferSize = 24576 (NICHT frame_size!)
// dwMaxVideoFrameSize     = 614400
// dwFrameInterval          = 333333 (100ns Einheiten = 30 FPS)
```

## Debugging im Hot Path

```c
// VERBOTEN im Streaming-Pfad:
printk("frame %d\n", count);     // ← Zerstört ISO Timing
LOG_DBG("dma complete");          // ← Mutex intern, blockiert

// ERLAUBT: GPIO-Toggle für Timing-Messung
gpio_pin_toggle_dt(&debug_pin);   // ← ~10 Zyklen, deterministisch

// EVALUIEREN: RTEdbg für binäres Tracing
// ~35 CPU-Zyklen pro Event, 4 Bytes Stack
// Binary Ringbuffer → Host-Dekodierung → VCD Waveform
// Siehe: https://github.com/RTEdbg/RTEdbg

// ERLAUBT außerhalb Hot Path (Init, Fehler):
LOG_INF("USB HS initialisiert");
LOG_ERR("DCD FIFO Konfiguration fehlgeschlagen: %d", ret);
```

## Fehlerbehandlung

```c
// Return-Code Pattern — IMMER
xi640_status_t xi640_uvc_init(void)
{
    xi640_status_t ret;

    ret = xi640_dcd_init();
    if (ret != XI640_STATUS_OK) {
        LOG_ERR("DCD Init fehlgeschlagen: %d", ret);
        return ret;
    }
    return XI640_STATUS_OK;
}

// Assertions für Invarianten (nur in Debug-Build)
__ASSERT(buf != NULL, "Buffer darf nicht NULL sein");
__ASSERT(IS_ALIGNED(buf, 32), "Buffer muss 32-Byte aligned sein");

// device_is_ready() IMMER prüfen nach DEVICE_DT_GET
const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(usart1));
if (!device_is_ready(dev)) {
    return XI640_STATUS_ERR_INIT;
}
```

## Dateistruktur

```
// Header: nur öffentliche API
inc/xi640_uvc.h    // Typen, Funktionsdeklarationen
inc/xi640_dcd.h

// Source: Implementierung
src/xi640_uvc.c
src/xi640_dcd.c

// Private Definitionen in .c Datei, nicht im Header
static void handle_sof_irq(void);  // static = datei-lokal
```

## Referenzen

Bei Bedarf die folgenden Referenz-Dateien in `references/` laden:
- `references/memory_map.md` — Vollständige AXISRAM/PSRAM Adressbereiche
- `references/usb_registers.md` — OTG HS Register-Details und FIFO-Berechnung
- `references/devicetree_patterns.md` — Overlay-Konventionen, Board-Overlays
