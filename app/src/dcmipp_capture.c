/**
 * @file dcmipp_capture.c
 * @brief DCMIPP Parallel-Interface Capture für ATTO640D-04 Thermalkamera
 *
 * Signalpfad: ATTO640D (Analog) → AD9649 ADC (14-Bit) → MachXO2 CPLD
 *             (Level-Shift 3.3V→1.8V, HSYNC +2 PSYNC Delay) → DCMIPP Pipe0
 *
 * Pipe0 (Dump Mode): Rohdaten unverändert in Frame-Buffer → DMA → AXISRAM1.
 *
 * ============================================================
 * DCMIPP_BYPASS_HAL = 1: Zephyr Video-Stack vollständig umgangen.
 *   Direkte HAL-Aufrufe statt video_set_format/enqueue/stream_start.
 *   Frame-Dequeue via Polling von CMSR2.P0FRAMEF (kein IRQ-Handler).
 *   Snapshot-Mode (CPTMODE=1): ein Frame pro CPTREQ → sauberes Double-Buffering.
 *
 * DCMIPP_BYPASS_HAL = 0: Zephyr Video-Stack (Referenz, #if 0 Backup).
 * ============================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/video.h>
#include <zephyr/cache.h>
#include <soc.h>  /* HAL: DCMIPP_TypeDef, HAL_DCMIPP_*, RCC APB5 Reset-Makros */

#include "dcmipp_capture.h"
/* Trace-System: nur aktiv wenn CONFIG_TRACE_UDP gesetzt (nicht in P22) */
#ifdef CONFIG_TRACE_UDP
#include "trace_udp.h"
#else
#define TRACE0(x) ((void)0)
#endif

LOG_MODULE_REGISTER(dcmipp_cap, LOG_LEVEL_INF);

/* ============================================================
 * 1 = HAL Bypass (aktiv), 0 = Zephyr Video-Stack (Backup)
 * ============================================================ */
#define DCMIPP_BYPASS_HAL  1

#define DCMIPP_NODE  DT_NODELABEL(pipe_dump)

/* Frame-Buffer in AXISRAM1 @ 0x34000000.
 * Subsection .a/.b erzwingt stabile Linker-Reihenfolge:
 *   buf_a=0x34000000, buf_b=0x34096000 */
static uint8_t frame_buf_a[DCMIPP_FRAME_SIZE]
    __attribute__((section(".dcmipp_frames.a"))) __aligned(64);
static uint8_t frame_buf_b[DCMIPP_FRAME_SIZE]
    __attribute__((section(".dcmipp_frames.b"))) __aligned(64);

static bool first_frame_logged;

/* ============================================================
 * Bypass-spezifische Variablen (nur für DCMIPP_BYPASS_HAL=1)
 * ============================================================ */
#if DCMIPP_BYPASS_HAL

static DCMIPP_HandleTypeDef hdcmipp_bypass;
/* 0 = buf_a ist aktuell, 1 = buf_b ist aktuell */
static uint8_t bypass_active_buf;

/* ============================================================
 * Zephyr-Stack-Variablen (nur für DCMIPP_BYPASS_HAL=0)
 * ============================================================ */
#else

static const struct device *dcmipp_dev;
static struct video_buffer vbuf_a;
static struct video_buffer vbuf_b;

#endif /* DCMIPP_BYPASS_HAL */


/* ============================================================
 * dcmipp_capture_get_buffers()
 * ============================================================ */
void dcmipp_capture_get_buffers(uint8_t **a, uint8_t **b)
{
    *a = frame_buf_a;
    *b = frame_buf_b;
}

/* ============================================================
 * dcmipp_capture_init()
 * ============================================================ */
int dcmipp_capture_init(void)
{
    /* Diagnose: PRCR-Zustand direkt nach Zephyr POST_KERNEL Init */
    {
        volatile uint32_t *prcr_diag = (volatile uint32_t *)(0x58002000UL + 0x104UL);
        uint32_t v = *prcr_diag;
        LOG_INF("DCMIPP PRCR nach Treiber-Init (vor Patches): 0x%08x "
                "(FORMAT=0x%02x EDM=%d PCKPOL=%d HSPOL=%d VSPOL=%d ENABLE=%d)",
                v,
                (unsigned)((v >> 16) & 0xFFU),
                (int)((v >> 10) & 0x7U),
                (int)((v >>  5) & 0x1U),
                (int)((v >>  6) & 0x1U),
                (int)((v >>  7) & 0x1U),
                (int)((v >> 14) & 0x1U));
    }

#if DCMIPP_BYPASS_HAL
    /* ============================================================
     * HAL BYPASS: Zephyr Video-Stack komplett umgangen.
     *
     * Konfiguration exakt wie Thomas' STM32-HAL-Referenz:
     *   FORMAT=MONO14B, EDM=14BIT, PCKPOL=RISING, HSPOL=LOW
     *   VSPOL=LOW, SYNCHRO=HARDWARE, SWAPCYCLES=DISABLE
     *
     * Vorteil: Kein Dummy-HAL_DCMIPP_PARALLEL_SetConfig() mit fehlerhafter
     * ENABLE=1 bei POST_KERNEL → keine Phasen-Akkumulation vor stream_start.
     * ============================================================ */

    /* 1. RCC: APB5 Clock sicherstellen */
    RCC->BUSENR  |= RCC_BUSENR_APB5EN;
    RCC->APB5ENR |= RCC_APB5ENR_DCMIPPEN;

    /* 2. Hardware-Reset: alle DCMIPP-Register auf Reset-Wert */
    __HAL_RCC_DCMIPP_FORCE_RESET();
    k_busy_wait(10);
    __HAL_RCC_DCMIPP_RELEASE_RESET();
    k_busy_wait(200);
    LOG_INF("BYPASS HW-Reset: RCC APB5 FORCE+RELEASE — PRCR=0x0 ENABLE=0");

    /* 3. HAL Init (setzt nur RAM-State: hdcmipp.State = HAL_DCMIPP_STATE_INIT) */
    hdcmipp_bypass.Instance = (DCMIPP_TypeDef *)0x58002000UL;
    if (HAL_DCMIPP_Init(&hdcmipp_bypass) != HAL_OK) {
        LOG_ERR("BYPASS HAL_DCMIPP_Init fehlgeschlagen");
        return -EIO;
    }

    /* 4. Parallel-Interface konfigurieren — exakt wie Thomas */
    DCMIPP_ParallelConfTypeDef par_cfg = {
        .Format           = DCMIPP_FORMAT_MONOCHROME_14B, /* 0x4D — monochrom 14-Bit */
        .SwapCycles       = DCMIPP_SWAPCYCLES_DISABLE,    /* 0 — kein Zyklus-Swap */
        .VSPolarity       = DCMIPP_VSPOLARITY_LOW,        /* 0 — VSYNC aktiv LOW  */
        .HSPolarity       = DCMIPP_HSPOLARITY_LOW,        /* 0 — HSYNC aktiv LOW  */
        .PCKPolarity      = DCMIPP_PCKPOLARITY_RISING,    /* RISING edge (PCKPOL=1) */
        .ExtendedDataMode = DCMIPP_INTERFACE_14BITS,      /* 14-Bit parallel bus  */
        .SynchroMode      = DCMIPP_SYNCHRO_HARDWARE,      /* 0 — Hardware HSYNC/VSYNC */
    };
    /* HAL forciert State=INIT vor dem Call (bekannter Treiber-Bug-Fix ist hier
     * nicht nötig, da wir gerade frisch initialisiert haben) */
    if (HAL_DCMIPP_PARALLEL_SetConfig(&hdcmipp_bypass, &par_cfg) != HAL_OK) {
        LOG_ERR("BYPASS HAL_DCMIPP_PARALLEL_SetConfig fehlgeschlagen");
        return -EIO;
    }
    {
        uint32_t prcr = hdcmipp_bypass.Instance->PRCR;
        LOG_INF("BYPASS PRCR nach PARALLEL_SetConfig: 0x%08x "
                "(FORMAT=0x%02x EDM=%d PCKPOL=%d HSPOL=%d ENABLE=%d)",
                prcr,
                (unsigned)((prcr >> 16) & 0xFFU),
                (int)((prcr >> 10) & 0x7U),
                (int)((prcr >>  5) & 0x1U),
                (int)((prcr >>  6) & 0x1U),
                (int)((prcr >> 14) & 0x1U));
        /* ENABLE ist nach HAL_DCMIPP_PARALLEL_SetConfig immer 1 (HAL-Verhalten).
         * Das ist OK — wir haben gerade frisch resettet, kein Phasen-Drift. */
    }

    /* 5. IPPLUG Client1 für Pipe0 DMA (identisch mit Zephyr stream_enable) */
    DCMIPP_IPPlugConfTypeDef ipplug_cfg = {
        .Client                     = DCMIPP_CLIENT1,
        .MemoryPageSize             = DCMIPP_MEMORY_PAGE_SIZE_1KBYTES,
        .Traffic                    = DCMIPP_TRAFFIC_BURST_SIZE_128BYTES,
        .MaxOutstandingTransactions = DCMIPP_OUTSTANDING_TRANSACTION_NONE,
        .DPREGStart                 = 0,
        .DPREGEnd                   = 511,
        .WLRURatio                  = 0xFU,
    };
    if (HAL_DCMIPP_SetIPPlugConfig(&hdcmipp_bypass, &ipplug_cfg) != HAL_OK) {
        LOG_ERR("BYPASS HAL_DCMIPP_SetIPPlugConfig fehlgeschlagen");
        return -EIO;
    }

    /* 6. Pipe0 konfigurieren (Dump: kein PixelPackerFormat, kein Pitch) */
    DCMIPP_PipeConfTypeDef pipe_cfg = {
        .FrameRate        = DCMIPP_FRAME_RATE_ALL,
        .PixelPipePitch   = 0,  /* Pipe0 Dump: kein Pitch-Register */
        .PixelPackerFormat = 0, /* Pipe0 Dump: nicht relevant */
    };
    if (HAL_DCMIPP_PIPE_SetConfig(&hdcmipp_bypass, DCMIPP_PIPE0, &pipe_cfg) != HAL_OK) {
        LOG_ERR("BYPASS HAL_DCMIPP_PIPE_SetConfig fehlgeschlagen");
        return -EIO;
    }

    /* 7. DMA-Zieladresse: buf_a als ersten Capture-Buffer */
    hdcmipp_bypass.Instance->P0PPM0AR1 = (uint32_t)frame_buf_a;
    bypass_active_buf = 0;  /* 0 = buf_a aktiv */

    /* 8. P0FSCR: PIPEN=1 (Pipe aktivieren) — Thomas: 0x80000000 */
    SET_BIT(hdcmipp_bypass.Instance->P0FSCR, DCMIPP_P0FSCR_PIPEN);

    /* 9. P0DCLMTR: kein Limit — Thomas: 0xFFFFFF */
    hdcmipp_bypass.Instance->P0DCLMTR = 0x00FFFFFFUL;

    /* 10. P0PPCR: PAD=0 (LSB-aligned, 14-Bit in Bits[13:0]) */
    CLEAR_BIT(hdcmipp_bypass.Instance->P0PPCR, (1UL << 5));

    LOG_INF("BYPASS P0FSCR=0x%08x P0DCLMTR=0x%06x P0PPCR=0x%08x P0PPM0AR1=0x%08x",
            hdcmipp_bypass.Instance->P0FSCR,
            hdcmipp_bypass.Instance->P0DCLMTR,
            hdcmipp_bypass.Instance->P0PPCR,
            hdcmipp_bypass.Instance->P0PPM0AR1);

    /* 11. Snapshot-Modus (CPTMODE=1): ein Frame pro CPTREQ, dann auto-clear.
     *     Vorteil vs. Continuous: sauberes Double-Buffering ohne Race-Window.
     *     Thomas verwendet Continuous (CPTMODE=0) mit ISR-Verwaltung —
     *     für unser Polling ist Snapshot kontrollierbarer.
     *     PRCR.ENABLE wurde bereits von HAL_DCMIPP_PARALLEL_SetConfig gesetzt. */
    SET_BIT(hdcmipp_bypass.Instance->P0FCTCR, DCMIPP_P0FCTCR_CPTMODE);  /* Snapshot */
    SET_BIT(hdcmipp_bypass.Instance->P0FCTCR, DCMIPP_P0FCTCR_CPTREQ);   /* ersten Frame anfordern */

    LOG_INF("BYPASS PRCR=0x%08x P0FCTCR=0x%08x",
            hdcmipp_bypass.Instance->PRCR,
            hdcmipp_bypass.Instance->P0FCTCR);
    LOG_INF("BYPASS Init OK — buf_a=%p buf_b=%p (CPTMODE=Snapshot, CPTREQ=1)",
            (void *)frame_buf_a, (void *)frame_buf_b);

    first_frame_logged = false;
    return 0;

#else /* DCMIPP_BYPASS_HAL == 0: alter Zephyr Video-Stack */

    int ret;

    dcmipp_dev = DEVICE_DT_GET(DCMIPP_NODE);
    if (!device_is_ready(dcmipp_dev)) {
        LOG_ERR("DCMIPP: Device nicht bereit");
        return -ENODEV;
    }
    LOG_INF("DCMIPP: Device bereit");

    struct video_format fmt = {
        .type        = VIDEO_BUF_TYPE_OUTPUT,
        .pixelformat = VIDEO_PIX_FMT_RGB565,
        .width       = DCMIPP_FRAME_WIDTH,
        .height      = DCMIPP_FRAME_HEIGHT,
        .pitch       = DCMIPP_FRAME_WIDTH * DCMIPP_FRAME_BPP,
    };
    ret = video_set_format(dcmipp_dev, &fmt);
    if (ret != 0) {
        LOG_ERR("DCMIPP: RGB565 Format abgelehnt (%d)", ret);
        return ret;
    }
    LOG_INF("DCMIPP: Format gesetzt — %dx%d, %u Bytes/Frame",
            DCMIPP_FRAME_WIDTH, DCMIPP_FRAME_HEIGHT, DCMIPP_FRAME_SIZE);

    /* Zephyr 4.3.x: video_enqueue() liest buf->index und greift auf
     * den internen video_buf[]-Pool zu. Ohne video_import_buffer() ist
     * pool[0].size=0 → DCMIPP-Treiber: 614400 > 0 → -EINVAL.
     * video_import_buffer() setzt pool[idx].size + .buffer korrekt. */
    ret = video_import_buffer(frame_buf_a, DCMIPP_FRAME_SIZE, &vbuf_a.index);
    if (ret != 0) { LOG_ERR("import buf_a: %d", ret); return ret; }
    vbuf_a.type = VIDEO_BUF_TYPE_OUTPUT;

    ret = video_import_buffer(frame_buf_b, DCMIPP_FRAME_SIZE, &vbuf_b.index);
    if (ret != 0) { LOG_ERR("import buf_b: %d", ret); return ret; }
    vbuf_b.type = VIDEO_BUF_TYPE_OUTPUT;

    ret = video_enqueue(dcmipp_dev, &vbuf_a);
    if (ret != 0) { LOG_ERR("enqueue a: %d", ret); return ret; }
    ret = video_enqueue(dcmipp_dev, &vbuf_b);
    if (ret != 0) { LOG_ERR("enqueue b: %d", ret); return ret; }

    ret = video_stream_start(dcmipp_dev, VIDEO_BUF_TYPE_OUTPUT);
    if (ret != 0) {
        LOG_ERR("DCMIPP: stream_start fehlgeschlagen: %d", ret);
        return ret;
    }

    /* SWAPCYCLES-Patch: Zephyr setzt SWAPCYCLES=1 für RGB565, aber MONO14B
     * ist ein 1-Zyklen-Format → SWAPCYCLES muss 0 sein, sonst Sägezahn-Artefakt
     * (jede zweite Zeile um 1-2 Pixel verschoben) an vertikalen Kanten.
     * PRCR Bit25 = SWAPCYCLES. Kein weiterer Patch nötig. */
    {
        volatile uint32_t *prcr_patch = (volatile uint32_t *)(0x58002000UL + 0x104UL);
        uint32_t val = *prcr_patch;
        val &= ~(1UL << 25);  /* SWAPCYCLES = 0 */
        *prcr_patch = val;
        LOG_INF("DCMIPP: SWAPCYCLES cleared (PRCR=0x%08x)", *prcr_patch);
    }

    /* Register-Dump nach stream_start */
    {
        volatile uint32_t *prcr = (volatile uint32_t *)0x58002104UL;
        uint32_t val = *prcr;
        LOG_INF("DCMIPP PRCR nach stream_start: 0x%08x "
                "(FORMAT=0x%02x EDM=%d PCKPOL=%d HSPOL=%d VSPOL=%d ENABLE=%d)",
                val,
                (unsigned)((val >> 16) & 0xFFU),
                (int)((val >> 10) & 0x7U),
                (int)((val >>  5) & 0x1U),
                (int)((val >>  6) & 0x1U),
                (int)((val >>  7) & 0x1U),
                (int)((val >> 14) & 0x1U));
    }
    {
        volatile uint32_t *p0ppcr = (volatile uint32_t *)0x580025C0UL;
        LOG_INF("DCMIPP P0PPCR=0x%08x", (uint32_t)*p0ppcr);
    }
    {
        volatile uint32_t *dclmtr = (volatile uint32_t *)0x580025B4UL;
        LOG_INF("DCMIPP P0DCLMTR=0x%06x", (uint32_t)*dclmtr);
    }
    {
        volatile uint32_t *p0fscr = (volatile uint32_t *)0x58002404UL;
        LOG_INF("DCMIPP P0FSCR=0x%08x (PIPEN=%d)",
                (uint32_t)*p0fscr,
                (int)(((uint32_t)*p0fscr >> 31) & 1U));
    }
    {
        volatile uint32_t *p0fctcr = (volatile uint32_t *)(0x58002000UL + 0x500UL);
        LOG_INF("DCMIPP P0FCTCR=0x%08x (CPTREQ=%d CPTMODE=%d)",
                (uint32_t)*p0fctcr,
                (int)(((uint32_t)*p0fctcr >> 3) & 1U),
                (int)(((uint32_t)*p0fctcr >> 2) & 1U));
    }

    first_frame_logged = false;
    LOG_INF("DCMIPP Zephyr-Stack: Streaming gestartet (buf_a=%p buf_b=%p)",
            (void *)frame_buf_a, (void *)frame_buf_b);
    return 0;

#endif /* DCMIPP_BYPASS_HAL */
}


/* ============================================================
 * dcmipp_capture_get_frame()
 * ============================================================ */
int dcmipp_capture_get_frame(uint8_t **data, uint32_t *size, int timeout_ms)
{
#if DCMIPP_BYPASS_HAL
    /* ---- Bypass: Polling auf CMSR2.P0FRAMEF (Bit9) ---- */
    int64_t deadline = (timeout_ms >= 0)
                       ? (k_uptime_get() + (int64_t)timeout_ms)
                       : INT64_MAX;

    while (!(hdcmipp_bypass.Instance->CMSR2 & DCMIPP_CMSR2_P0FRAMEF)) {
        if (k_uptime_get() >= deadline) {
            return -ETIMEDOUT;
        }
        k_yield();
    }

    /* P0FRAMEF-Flag loeschen (write-1-to-clear in CMFCR) */
    hdcmipp_bypass.Instance->CMFCR = DCMIPP_CMFCR_CP0FRAMEF;

    /* Completed Buffer bestimmen */
    uint8_t *completed_buf = (bypass_active_buf == 0) ? frame_buf_a : frame_buf_b;

    /* Naechsten Buffer vorbereiten und CPTREQ sofort setzen (Snapshot) */
    bypass_active_buf ^= 1;
    uint8_t *next_buf = (bypass_active_buf == 0) ? frame_buf_a : frame_buf_b;
    hdcmipp_bypass.Instance->P0PPM0AR1 = (uint32_t)next_buf;
    SET_BIT(hdcmipp_bypass.Instance->P0FCTCR, DCMIPP_P0FCTCR_CPTREQ);

    TRACE0(EVT_FRAME_DEQUEUED);

    /* D-Cache invalidieren: DMA hat direkt in AXISRAM1 geschrieben */
    TRACE0(EVT_CACHE_INV_START);
    sys_cache_data_invd_range(completed_buf, DCMIPP_FRAME_SIZE);
    TRACE0(EVT_CACHE_INV_DONE);

    /* Kein memcpy — completed_buf direkt zurückgeben (AXISRAM1, kein PSRAM).
     * thermal_stream liest aus buf_a oder buf_b während DCMIPP in next_buf schreibt.
     * Frame-Zeit ~20ms, TX-Zeit ~11ms → kein Overlap. */
    if (!first_frame_logged) {
        first_frame_logged = true;
        uint16_t *f = (uint16_t *)completed_buf;
        LOG_INF("BYPASS: erster Frame buf=%p CMFRCR=%u",
                (void *)completed_buf,
                hdcmipp_bypass.Instance->CMFRCR);
        LOG_INF("  [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u",
                f[0]&0x3FFF, f[1]&0x3FFF, f[2]&0x3FFF,
                f[3]&0x3FFF, f[4]&0x3FFF);
    }

    *data = completed_buf;
    *size = DCMIPP_FRAME_SIZE;
    return 0;

#else /* DCMIPP_BYPASS_HAL == 0: Zephyr Video-Stack */

#error "DCMIPP_BYPASS_HAL=0 nicht implementiert in P22 — DCMIPP_BUF_C undefiniert. Setze DCMIPP_BYPASS_HAL=1 in CMakeLists.txt."

    struct video_buffer *vbuf;
    k_timeout_t timeout;

    if (timeout_ms < 0) {
        timeout = K_FOREVER;
    } else if (timeout_ms == 0) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_MSEC(timeout_ms);
    }

    int ret = video_dequeue(dcmipp_dev, &vbuf, timeout);
    if (ret != 0) {
        return ret;
    }

    TRACE0(EVT_FRAME_DEQUEUED);

    TRACE0(EVT_CACHE_INV_START);
    sys_cache_data_invd_range(vbuf->buffer, DCMIPP_FRAME_SIZE);
    TRACE0(EVT_CACHE_INV_DONE);

    /* Minimal: Kopie nach Buf C (PSRAM @ 0x90500000), sofort re-enqueue */
    memcpy(DCMIPP_BUF_C, vbuf->buffer, vbuf->bytesused);
    video_enqueue(dcmipp_dev, vbuf);

    if (!first_frame_logged) {
        first_frame_logged = true;
        LOG_INF("DCMIPP: erster Frame → BufC=%p bytesused=%u",
                (void *)DCMIPP_BUF_C, vbuf->bytesused);
    }

    *data = DCMIPP_BUF_C;
    *size = vbuf->bytesused;
    return 0;

#endif /* DCMIPP_BYPASS_HAL */
}
