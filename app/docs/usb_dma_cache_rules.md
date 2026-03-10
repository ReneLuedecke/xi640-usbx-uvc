# Xi640 — USB / DMA / Cache Regeln

## 6 kritische USBX Performance-Stellen

### 1. dwMaxPayloadTransferSize
```c
// FALSCH — frame_size ist zu groß für einen Transfer
.dwMaxPayloadTransferSize = frame_size;

// RICHTIG — maximale USB-Payload pro Transfer
.dwMaxPayloadTransferSize = 24576;  // = 1024 * 3 * 8
```

### 2. Transfer-Größe
```c
// FALSCH — 512 Bytes pro Paket = 1200 Transfers pro Frame
ux_device_class_video_write_payload(video, buf, 512);

// RICHTIG — 16–32 KB Blöcke
ux_device_class_video_write_payload(video, buf, 16384);
```

### 3. ISO Descriptor wMaxPacketSize
```c
// USB HS ISO High-Bandwidth: 1024 Bytes × 3 Transactions
.wMaxPacketSize = 0x1400,  // Bits 12:11 = 10b = 3 transactions
```

### 4. FIFO Konfiguration (OTG HS)
```c
// Gesamt: 4096 Words verfügbar
USB_OTG_HS->GRXFSIZ    = 1024;  // RX FIFO
USB_OTG_HS->DIEPTXF0   = (512  << 16) | 1024;  // EP0 TX
USB_OTG_HS->DIEPTXF[0] = (2048 << 16) | 1536;  // EP1 ISO TX
// Belegt: 1024 + 512 + 2048 = 3584 / 4096 Words
```

### 5. DMA aktivieren
```c
// GAHBCFG: DMA enable + HBSTLEN = INCR16
USB_OTG_HS->GAHBCFG = USB_OTG_GAHBCFG_DMAEN
                     | (0x7 << USB_OTG_GAHBCFG_HBSTLEN_Pos)
                     | USB_OTG_GAHBCFG_GINT;
```

### 6. SOF Synchronisation (Even/Odd Frame)
```c
// Im SOF IRQ — verhindert Frame-Glitches
void OTG_HS_IRQHandler(void) {
    if (USB_OTG_HS->GINTSTS & USB_OTG_GINTSTS_SOF) {
        uint32_t fn = (USB_OTG_HS->DSTS & USB_OTG_DSTS_FNSOF) >> 8;
        if (fn & 1) {
            USB_OTG_HS->DIEPCTL1 |= USB_OTG_DIEPCTL_SODDFRM;
        } else {
            USB_OTG_HS->DIEPCTL1 |= USB_OTG_DIEPCTL_SEVNFRM;
        }
    }
}
```

---

## ux_port.c — 6 kritische Fixes

### Fix 1: Kein k_malloc in Thread-Create
```c
// FALSCH — k_malloc in IRQ/Init-Context verboten
UX_THREAD *thread = k_malloc(sizeof(UX_THREAD));

// RICHTIG — statisch in UX_THREAD einbetten
typedef struct UX_THREAD_STRUCT {
    struct k_thread thread;
    k_thread_stack_t *stack;
    // ...
} UX_THREAD;
```

### Fix 2: Thread suspend korrekt
```c
// FALSCH — suspendiert den CALLER
k_sleep(K_FOREVER);

// RICHTIG — suspendiert den Ziel-Thread
k_thread_suspend(tid);
```

### Fix 3: Thread resume korrekt
```c
// FALSCH — bricht nur k_sleep ab
k_wakeup(tid);

// RICHTIG — resumt suspendierten Thread
k_thread_resume(tid);
```

### Fix 4: Timeout-Mapping
```c
// UX_WAIT_FOREVER muss auf K_FOREVER gemappt werden
#define UX_WAIT_FOREVER  0xFFFFFFFFUL

static inline k_timeout_t ux_to_zephyr_timeout(ULONG ticks) {
    if (ticks == UX_WAIT_FOREVER) return K_FOREVER;
    return K_TICKS(ticks);
}
```

### Fix 5: Cache Alignment (Cortex-M55 = 32 Byte)
```c
// Alle DMA-Puffer müssen 32-Byte aligned sein
#define UX_CACHE_ALIGN  __attribute__((aligned(32)))

UX_CACHE_ALIGN static uint8_t iso_buffer[UX_ISO_BUF_SIZE];
```

### Fix 6: Keine Mutexe aus ISR
```c
// FALSCH — Mutex aus ISR ist undefined behavior
k_mutex_lock(&ux_mutex, K_NO_WAIT);  // in ISR

// RICHTIG — nur Semaphore sind ISR-safe
k_sem_give(&ux_sem);  // in ISR — ok
k_sem_take(&ux_sem, K_FOREVER);  // in Thread — ok
```

---

## STM32N6 spezifische Hinweise

- **VENC braucht DCMIPP Clock** — auch ohne Kamera! `__HAL_RCC_DCMIPP_CLK_ENABLE()` in `stm32_venc_enable_clock()`
- **ISR FPU Context** — VENC IRQ immer mit `IRQ_CONNECT` (nicht `ISR_DIRECT_DECLARE`)
- **D-Cache** — muss explizit aktiviert werden, sonst nur ~15 MB/s Speicherbandbreite
- **Software-Breakpoints** — nur in AXISRAM möglich, nicht in externem Flash (0x70xxxxxx)
- **PSRAM Reset** — nach Power-Cycle: 0xFF Global Reset in SPI-Mode vor HexaSPI-Switch
