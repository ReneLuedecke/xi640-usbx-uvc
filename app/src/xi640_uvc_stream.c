/**
 * @file    xi640_uvc_stream.c
 * @brief   Xi640 UVC Colorbar Streaming — Implementierung
 *
 * Phase 5: Statisches YUYV Farbtestbild (640x480) ueber USB UVC ISO
 * streamen. Verifiziert: Windows erkennt UVC-Geraet und zeigt Farbbild.
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#include "xi640_uvc_stream.h"
#include "xi640_uvc_descriptors.h"
#include "xi640_dcd.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ux_api.h"
#include "ux_device_class_video.h"
#include "ux_device_stack.h"

LOG_MODULE_REGISTER(xi640_uvc, CONFIG_LOG_DEFAULT_LEVEL);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Konfiguration                                                          */
/* ──────────────────────────────────────────────────────────────────────── */

#define XI640_UVC_FRAME_WIDTH     640U
#define XI640_UVC_FRAME_HEIGHT    480U
#define XI640_UVC_FRAME_SIZE      (XI640_UVC_FRAME_WIDTH * XI640_UVC_FRAME_HEIGHT * 2U)

/** Maximale Payload pro USB-Transfer (inkl. UVC Header).                */
/** Entspricht dwMaxPayloadTransferSize = 24576 = 0x6000                  */
#define XI640_UVC_MAX_PAYLOAD     24576U

/** USBX Payload Buffer Groesse: +4 fuer internen payload_length ULONG.  */
#define XI640_UVC_PAYLOAD_BUF_SIZE  (XI640_UVC_MAX_PAYLOAD + 4U)

/** Anzahl Double-Buffer Slots im USBX Payload Ring.                     */
#define XI640_UVC_PAYLOAD_BUF_NB  2U

/** USBX Memory Pool Groesse (128 KB).                                   */
#define XI640_USBX_POOL_SIZE      131072U

/** Anzahl TV-Farb-Teststreifen (8 Klassische Colorbars).                */
#define XI640_COLORBAR_NB         8U

/* ──────────────────────────────────────────────────────────────────────── */
/*  Puffer-Adressen in AXISRAM3+4                                         */
/*                                                                         */
/*  FSBL RAM (AXISRAM2, 511 KB) reicht nicht fuer diese Puffer:           */
/*    frame_buf  = 614400 B (600 KB)                                       */
/*    usbx_pool  = 131072 B (128 KB)                                       */
/*    Summe      = 745472 B (728 KB)                                       */
/*                                                                         */
/*  AXISRAM3 (0x34200000, 448 KB) + AXISRAM4 (0x34270000, 448 KB)        */
/*  sind physisch kontiguierlich — zusammen 896 KB.                        */
/*  Die Banken werden vom RAMCFG-Treiber eingeschaltet, weil das Overlay  */
/*  &axisram3 / &axisram4 mit status = "okay" aktiviert.                  */
/*                                                                         */
/*  Feste Adresse statt section()-Attribut — kein Linker-Konflikt         */
/*  mit dem BSS-Zero-Init in AXISRAM2 (zephyr,sram).                      */
/* ──────────────────────────────────────────────────────────────────────── */

/** AXISRAM3 Basisadresse (physisch, aus STM32N6 Reference Manual). */
#define XI640_AXISRAM3_BASE  0x34200000U

/** Frame Buffer: ab AXISRAM3-Start — 614400 B.
 *  614400 = 0x96000, endet bei 0x34296000 (mitten in AXISRAM4). */
static uint8_t * const frame_buf =
    (uint8_t *)XI640_AXISRAM3_BASE;

/** USBX Pool: direkt nach frame_buf — 131072 B.
 *  614400 ist exakt 32-Byte aligned (614400 / 32 = 19200).
 *  Pool liegt bei 0x34296000, endet bei 0x342B6000 (innerhalb AXISRAM4). */
static uint8_t * const usbx_pool =
    (uint8_t *)(XI640_AXISRAM3_BASE + XI640_UVC_FRAME_SIZE);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Modul-interner Zustand                                                 */
/* ──────────────────────────────────────────────────────────────────────── */

static UX_DEVICE_CLASS_VIDEO        *g_video_instance = NULL;
static UX_DEVICE_CLASS_VIDEO_STREAM *g_video_stream   = NULL;

/** 1 wenn Host VS Alt=1 gesetzt hat (Streaming aktiv), sonst 0.        */
static atomic_t g_stream_active = ATOMIC_INIT(0);

/** UVC Payload Frame-ID Bit (togglet nach jedem vollstaendigen Frame).  */
static uint8_t g_fid = 0U;

/** Offset im Frame-Buffer des naechsten zu sendenden Bytes.             */
static uint32_t g_frame_offset = 0U;

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX Probe/Commit Control Struktur                                    */
/* ──────────────────────────────────────────────────────────────────────── */

static UX_DEVICE_CLASS_VIDEO_PROBE_COMMIT_CONTROL g_probe_commit;

static void xi640_uvc_init_probe_commit(void)
{
    memset(&g_probe_commit, 0, sizeof(g_probe_commit));

    g_probe_commit.bmHint.value    = 0x0001U;   /* dwFrameInterval fixieren */
    g_probe_commit.bFormatIndex    = 1U;
    g_probe_commit.bFrameIndex     = 1U;
    g_probe_commit.dwFrameInterval = 333333UL;  /* 30 FPS in 100ns-Einheiten */

    /* dwMaxVideoFrameSize = 614400 = 0x00096000 (Little-Endian) */
    g_probe_commit.dwMaxVideoFrameSize[0] = 0x00U;
    g_probe_commit.dwMaxVideoFrameSize[1] = 0x60U;
    g_probe_commit.dwMaxVideoFrameSize[2] = 0x09U;
    g_probe_commit.dwMaxVideoFrameSize[3] = 0x00U;

    /* dwMaxPayloadTransferSize = 24576 = 0x00006000 (Little-Endian) */
    g_probe_commit.dwMaxPayloadTransferSize[0] = 0x00U;
    g_probe_commit.dwMaxPayloadTransferSize[1] = 0x60U;
    g_probe_commit.dwMaxPayloadTransferSize[2] = 0x00U;
    g_probe_commit.dwMaxPayloadTransferSize[3] = 0x00U;

    g_probe_commit.bmFramingInfo.value = 0x03U; /* FID required, EOF may present */
    g_probe_commit.bPreferedVersion    = 1U;
    g_probe_commit.bMinVersion         = 1U;
    g_probe_commit.bMaxVersion         = 1U;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Colorbar Testbild Generator (YUYV)                                    */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Befuellt frame_buf mit einem YUYV Farbtestbild (8 Streifen).
 *
 * Klassische TV-Farbbalkenmuster — je 80 Pixel breit, 480 Zeilen hoch.
 * Kein Log im Hot Path — wird einmalig beim Modul-Init aufgerufen.
 */
static void xi640_uvc_generate_colorbar(void)
{
    /* YUYV-Werte fuer 8 klassische TV-Farb-Teststreifen            */
    /* {Y0, U, Y1, V} — zwei Pixel teilen U/V                      */
    static const uint8_t bars[XI640_COLORBAR_NB][4] = {
        { 235, 128, 235, 128 }, /* Weiss  */
        { 210,  16, 210, 146 }, /* Gelb   */
        { 170, 166, 170,  16 }, /* Cyan   */
        { 145,  54, 145,  34 }, /* Gruen  */
        { 106, 202, 106, 222 }, /* Magenta*/
        {  81,  90,  81, 240 }, /* Rot    */
        {  41, 240,  41, 110 }, /* Blau   */
        {  16, 128,  16, 128 }, /* Schwarz*/
    };

    const uint32_t bar_width_px = XI640_UVC_FRAME_WIDTH / XI640_COLORBAR_NB;

    for (uint32_t y = 0U; y < XI640_UVC_FRAME_HEIGHT; y++) {
        for (uint32_t x = 0U; x < XI640_UVC_FRAME_WIDTH; x += 2U) {
            uint32_t bar = x / bar_width_px;
            if (bar >= XI640_COLORBAR_NB) {
                bar = XI640_COLORBAR_NB - 1U;
            }

            uint32_t offset = (y * XI640_UVC_FRAME_WIDTH + x) * 2U;
            frame_buf[offset + 0U] = bars[bar][0]; /* Y0 */
            frame_buf[offset + 1U] = bars[bar][1]; /* U  */
            frame_buf[offset + 2U] = bars[bar][2]; /* Y1 */
            frame_buf[offset + 3U] = bars[bar][3]; /* V  */
        }
    }

    /* Cache-Flush: CPU hat frame_buf geschrieben, DMA soll es lesen.   */
    xi640_dcd_cache_flush(frame_buf, XI640_UVC_FRAME_SIZE);
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX Callback: Video Class Activate/Deactivate                        */
/* ──────────────────────────────────────────────────────────────────────── */

static void xi640_uvc_activate(void *instance)
{
    g_video_instance = (UX_DEVICE_CLASS_VIDEO *)instance;

    if (ux_device_class_video_stream_get(g_video_instance, 0U,
                                         &g_video_stream) != UX_SUCCESS) {
        LOG_ERR("Video Stream 0 nicht abrufbar");
        g_video_stream = NULL;
    } else {
        LOG_INF("UVC aktiviert — Stream bereit");
    }
}

static void xi640_uvc_deactivate(void *instance)
{
    (void)instance;
    atomic_set(&g_stream_active, 0);
    g_video_instance = NULL;
    g_video_stream   = NULL;
    g_frame_offset   = 0U;
    LOG_INF("UVC deaktiviert");
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX Callback: VS Stream Change (Alt-Setting Wechsel)                 */
/* ──────────────────────────────────────────────────────────────────────── */

static void xi640_uvc_stream_change(UX_DEVICE_CLASS_VIDEO_STREAM *stream,
                                    ULONG alt)
{
    if (alt == 1U) {
        /* Host hat Alt=1 gesetzt — Streaming starten */
        g_frame_offset = 0U;
        g_fid          = 0U;

        if (ux_device_class_video_transmission_start(stream) != UX_SUCCESS) {
            LOG_ERR("Transmission Start fehlgeschlagen");
            return;
        }
        atomic_set(&g_stream_active, 1);
        LOG_INF("VS Streaming gestartet (Alt=1)");
    } else {
        /* Host hat Alt=0 gesetzt — Streaming stoppen */
        atomic_set(&g_stream_active, 0);
        g_frame_offset = 0U;
        LOG_INF("VS Streaming gestoppt (Alt=0)");
    }
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX Callback: VS Stream Request (Probe/Commit Verhandlung)           */
/* ──────────────────────────────────────────────────────────────────────── */

static UINT xi640_uvc_stream_request(UX_DEVICE_CLASS_VIDEO_STREAM *stream,
                                     UX_SLAVE_TRANSFER *transfer)
{
    (void)stream;

    UCHAR  request  = transfer->ux_slave_transfer_request_setup[UX_SETUP_REQUEST];
    UCHAR  value_cs = transfer->ux_slave_transfer_request_setup[UX_SETUP_VALUE + 1];
    UCHAR *buf      = transfer->ux_slave_transfer_request_data_pointer;
    ULONG  req_len  = transfer->ux_slave_transfer_request_requested_length;
    UINT   status;

    /* Wir behandeln VS_PROBE_CONTROL und VS_COMMIT_CONTROL identisch.  */
    if (value_cs != UX_DEVICE_CLASS_VIDEO_VS_PROBE_CONTROL &&
        value_cs != UX_DEVICE_CLASS_VIDEO_VS_COMMIT_CONTROL) {
        return UX_ERROR;
    }

    switch (request) {
    case UX_DEVICE_CLASS_VIDEO_SET_CUR:
        /* Host uebergibt Probe/Commit — wir akzeptieren stillschweigend */
        return UX_SUCCESS;

    case UX_DEVICE_CLASS_VIDEO_GET_CUR:
    case UX_DEVICE_CLASS_VIDEO_GET_MIN:
    case UX_DEVICE_CLASS_VIDEO_GET_MAX:
    case UX_DEVICE_CLASS_VIDEO_GET_DEF:
        _ux_utility_memory_copy(buf, &g_probe_commit, sizeof(g_probe_commit));
        status = _ux_device_stack_transfer_request(transfer,
                     sizeof(g_probe_commit), req_len);
        return status;

    case UX_DEVICE_CLASS_VIDEO_GET_INFO:
        *buf = (UCHAR)(UX_DEVICE_CLASS_VIDEO_INFO_GET_REQUEST_SUPPORT |
                       UX_DEVICE_CLASS_VIDEO_INFO_SET_REQUEST_SUPPORT);
        status = _ux_device_stack_transfer_request(transfer, 1U, req_len);
        return status;

    default:
        return UX_ERROR;
    }
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Payload Chunk senden (Hot Path — kein LOG!)                           */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Naechsten Payload-Chunk an USBX uebergeben.
 *
 * Holt freien Buffer, befuellt ihn mit UVC Header + YUYV Daten,
 * Cache-Flush, dann ux_device_class_video_write_payload_commit().
 *
 * @return  0 kein Buffer verfuegbar (Retry), 1 Chunk gesendet
 */
static int xi640_uvc_send_next_chunk(void)
{
    UCHAR *payload_buf;
    ULONG  max_len;

    if (ux_device_class_video_write_payload_get(g_video_stream,
                                                &payload_buf,
                                                &max_len) != UX_SUCCESS) {
        return 0; /* Kein freier Buffer — warten */
    }

    /* Wie viel YUYV-Daten passt nach dem UVC-Header rein? */
    uint32_t remaining = XI640_UVC_FRAME_SIZE - g_frame_offset;
    uint32_t data_max  = (uint32_t)(max_len) - 2U; /* 2 Byte UVC-Header */
    uint32_t data_len  = (remaining < data_max) ? remaining : data_max;
    uint8_t  eof       = (g_frame_offset + data_len >= XI640_UVC_FRAME_SIZE)
                         ? 1U : 0U;

    /* UVC Payload Header (2 Byte minimal) */
    payload_buf[0] = 0x02U;                          /* bHeaderLength = 2 */
    payload_buf[1] = g_fid | (uint8_t)(eof << 1U);  /* FID | EOF */

    /* YUYV Daten kopieren */
    memcpy(payload_buf + 2U, frame_buf + g_frame_offset, data_len);

    /* Cache-Flush: Payload-Buffer vor DMA TX schreiben */
    xi640_dcd_cache_flush(payload_buf, 2U + data_len);

    ux_device_class_video_write_payload_commit(g_video_stream, 2U + data_len);

    g_frame_offset += data_len;

    if (eof) {
        g_fid ^= 1U;       /* Frame-ID toggeln fuer naechsten Frame */
        g_frame_offset = 0U;
    }

    return 1;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX Task-Thread (Prio 1 — hoeher als Streaming)                      */
/*                                                                         */
/*  ux_system_tasks_run() MUSS so schnell wie moeglich aufgerufen werden.  */
/*  Im UX_STANDALONE-Modus verarbeitet USBX SETUP-Pakete (Enumeration)    */
/*  und Video-Write-Tasks ausschliesslich in diesem Call — NICHT im ISR.  */
/*  Der Host erwartet eine Descriptor-Antwort innerhalb von ~500 µs.      */
/*  k_yield() statt k_sleep() damit der Thread sofort zurueckkehrt.       */
/* ──────────────────────────────────────────────────────────────────────── */

K_THREAD_STACK_DEFINE(xi640_usbx_task_stack, 2048);
static struct k_thread xi640_usbx_task_thread;

static void xi640_usbx_task_thread_fn(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    while (1) {
        ux_system_tasks_run();
        k_yield(); /* CPU-Slot abgeben, sofort wieder bereit */
    }
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Streaming Thread (Prio 2)                                              */
/* ──────────────────────────────────────────────────────────────────────── */

K_THREAD_STACK_DEFINE(xi640_uvc_stream_stack, 4096);
static struct k_thread xi640_uvc_stream_thread;

static void xi640_uvc_stream_thread_fn(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    while (1) {
        if (!atomic_get(&g_stream_active) || g_video_stream == NULL) {
            /* Noch kein Streaming — kurz abgeben, nicht schlafen */
            k_yield();
            continue;
        }

        /* Naechsten Payload-Chunk an USBX uebergeben */
        if (!xi640_uvc_send_next_chunk()) {
            /* Kein freier Buffer — kurz yield dann retry */
            k_yield();
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  USBX System Callback                                                   */
/* ──────────────────────────────────────────────────────────────────────── */

static UINT xi640_uvc_device_change(ULONG event)
{
    switch (event) {
    case UX_DEVICE_CONNECTION:
        LOG_INF("USB: Verbunden");
        break;
    case UX_DEVICE_DISCONNECTION:
        LOG_INF("USB: Getrennt");
        atomic_set(&g_stream_active, 0);
        g_frame_offset = 0U;
        break;
    default:
        break;
    }
    return UX_SUCCESS;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Oeffentliche API                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

xi640_uvc_status_t xi640_uvc_init(void)
{
    UINT ux_ret;

    /* ─── Colorbar Testbild generieren ────────────────────────────────── */
    xi640_uvc_generate_colorbar();
    LOG_INF("Colorbar YUYV generiert (%u Bytes)", XI640_UVC_FRAME_SIZE);

    /* ─── Probe/Commit Struktur initialisieren ─────────────────────────── */
    xi640_uvc_init_probe_commit();

    /* ─── USBX Pool nullen (kein automatisches BSS-Zero-Init in AXISRAM3) */
    memset(usbx_pool, 0, XI640_USBX_POOL_SIZE);

    /* ─── USBX System initialisieren ─────────────────────────────────── */
    ux_ret = ux_system_initialize(usbx_pool, XI640_USBX_POOL_SIZE,
                                  UX_NULL, 0U);
    if (ux_ret != UX_SUCCESS) {
        LOG_ERR("ux_system_initialize fehlgeschlagen: 0x%x", ux_ret);
        return XI640_UVC_ERR_SYSTEM_INIT;
    }
    LOG_INF("USBX System initialisiert (Pool: %u Bytes)", XI640_USBX_POOL_SIZE);

    /* ─── USBX Device Stack initialisieren ────────────────────────────── */
    ux_ret = ux_device_stack_initialize(
        (UCHAR *)xi640_uvc_hs_framework, xi640_uvc_hs_framework_len,
        (UCHAR *)xi640_uvc_fs_framework, xi640_uvc_fs_framework_len,
        (UCHAR *)xi640_uvc_string_framework, xi640_uvc_string_framework_len,
        (UCHAR *)xi640_uvc_language_id_framework,
        xi640_uvc_language_id_framework_len,
        xi640_uvc_device_change);
    if (ux_ret != UX_SUCCESS) {
        LOG_ERR("ux_device_stack_initialize fehlgeschlagen: 0x%x", ux_ret);
        return XI640_UVC_ERR_STACK_INIT;
    }
    LOG_INF("USBX Device Stack initialisiert");

    /* ─── UVC Video Class Parameter konfigurieren ─────────────────────── */
    static UX_DEVICE_CLASS_VIDEO_STREAM_PARAMETER stream_param;
    static UX_DEVICE_CLASS_VIDEO_PARAMETER        video_param;

    memset(&stream_param, 0, sizeof(stream_param));
    memset(&video_param,  0, sizeof(video_param));

    /* Standalone Modus: task_function statt thread_entry               */
    stream_param.ux_device_class_video_stream_parameter_task_function =
        _ux_device_class_video_write_task_function;

    stream_param.ux_device_class_video_stream_parameter_max_payload_buffer_size =
        XI640_UVC_PAYLOAD_BUF_SIZE;
    stream_param.ux_device_class_video_stream_parameter_max_payload_buffer_nb =
        XI640_UVC_PAYLOAD_BUF_NB;

    stream_param.ux_device_class_video_stream_parameter_callbacks
        .ux_device_class_video_stream_change   = xi640_uvc_stream_change;
    stream_param.ux_device_class_video_stream_parameter_callbacks
        .ux_device_class_video_stream_request  = xi640_uvc_stream_request;
    stream_param.ux_device_class_video_stream_parameter_callbacks
        .ux_device_class_video_stream_payload_done = NULL;

    video_param.ux_device_class_video_parameter_master_interface = 0U;
    video_param.ux_device_class_video_parameter_callbacks
        .ux_slave_class_video_instance_activate   = xi640_uvc_activate;
    video_param.ux_device_class_video_parameter_callbacks
        .ux_slave_class_video_instance_deactivate = xi640_uvc_deactivate;
    video_param.ux_device_class_video_parameter_callbacks
        .ux_device_class_video_request            = NULL;
    video_param.ux_device_class_video_parameter_streams_nb = 1U;
    video_param.ux_device_class_video_parameter_streams    = &stream_param;

    /* ─── Video Class registrieren ────────────────────────────────────── */
    ux_ret = ux_device_stack_class_register(
        _ux_system_device_class_video_name,
        ux_device_class_video_entry,
        1U,   /* Konfigurationsnummer */
        0U,   /* Interface-Nummer (VC IAD Master Interface = 0) */
        &video_param);
    if (ux_ret != UX_SUCCESS) {
        LOG_ERR("ux_device_stack_class_register fehlgeschlagen: 0x%x", ux_ret);
        return XI640_UVC_ERR_CLASS_REGISTER;
    }
    LOG_INF("UVC Video Class registriert");

    return XI640_UVC_OK;
}

void xi640_uvc_stream_start(void)
{
    /* ─── USBX Task-Thread starten (Prio 1, hoeher als Streaming) ─────── */
    /* Dieser Thread pumpt ux_system_tasks_run() so schnell wie moeglich.  */
    /* Ohne ihn wuerden Enumeration-SETUP-Pakete zu spaet beantwortet.     */
    k_thread_create(&xi640_usbx_task_thread,
                    xi640_usbx_task_stack,
                    K_THREAD_STACK_SIZEOF(xi640_usbx_task_stack),
                    xi640_usbx_task_thread_fn,
                    NULL, NULL, NULL,
                    1,          /* Prioritaet 1 — hoeher als Streaming-Thread */
                    0,
                    K_NO_WAIT);
    k_thread_name_set(&xi640_usbx_task_thread, "usbx_tasks");
    LOG_INF("USBX Task-Thread gestartet (Prio=1, Stack=2048)");

    /* ─── Streaming-Thread starten (Prio 2) ───────────────────────────── */
    k_thread_create(&xi640_uvc_stream_thread,
                    xi640_uvc_stream_stack,
                    K_THREAD_STACK_SIZEOF(xi640_uvc_stream_stack),
                    xi640_uvc_stream_thread_fn,
                    NULL, NULL, NULL,
                    2,          /* Prioritaet 2 */
                    0,
                    K_NO_WAIT);
    k_thread_name_set(&xi640_uvc_stream_thread, "uvc_stream");
    LOG_INF("UVC Streaming-Thread gestartet (Prio=2, Stack=4096)");
}
