/**
 * @file    usbx_zephyr.c
 * @brief   USBX Init + UVC/CDC Klassen-Registrierung — Xi640
 *
 * Adaptiert aus ST x-cube-n6-camera-capture:
 *   Lib/Camera_Middleware/ISP_Library/isp/USBX/Src/usbx/usbx.c
 *
 * Aenderungen gegenueber ST Original:
 *   - PCD-Handle intern via xi640_dcd_get_pcd_handle() (kein extern)
 *   - _ux_dcd_stm32_initialize() via xi640_dcd_register_usbx()
 *   - printf() → LOG_INF/LOG_ERR (kein stdio in Embedded)
 *   - UVC_handle_setup_request() lokal implementiert (kein UVCL)
 *   - UVC_StartNewFrameTransmission/UpdateOnFlyCtx/AbortOnFlyCtx lokal
 *   - YUYV Colorbar Testbild in AXISRAM3 (0x34200000)
 *   - usbx_init() Signatur: int usbx_init(uvc_ctx_t *p_ctx)
 *
 * Copyright (c) 2025 STMicroelectronics / 2026 Optris GmbH.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <assert.h>
#include <string.h>

#include "usbx.h"
#include "stm32n6xx_hal.h"
#include "usb_desc.h"
#include "usb_desc_internal.h"
#include "usbx_codes.h"
#include "xi640_dcd.h"

#include "ux_api.h"
#include "ux_dcd_stm32.h"
#include "ux_device_class_video.h"
#include "ux_device_class_cdc_acm.h"

LOG_MODULE_REGISTER(usbx_zephyr, LOG_LEVEL_INF);

/* ── Memory Layout in AXISRAM3/4 (DMA-zugaenglich) ─────────────────────────
 *
 * USB OTG DMA-Master hat auf STM32N6 KEINEN Zugriff auf AXISRAM2 (BSS).
 * Alle Puffer die der DMA liest/schreibt MUESSEN in AXISRAM3/4 liegen.
 *
 * AXISRAM3: 0x34200000, 512 KB
 * AXISRAM4: 0x34280000, 512 KB  (beide per DTS Overlay aktiviert)
 *
 * Belegung (keine Ueberlappung mit hpcd @ 0x34296000):
 *   0x34200000  Frame Buffer A  (614400 B = 0x96000) — endet 0x34296000
 *   0x34296000  hpcd            (in xi640_dcd.c)
 *   0x342A0000  usbx_mem_pool   (32 KB)
 *   0x342A8000  usb_desc_hs/fs  (2x 512 B)
 *   0x342A8400  usb_dev_strings (512 B)
 *   0x342A8600  usb_dev_langid  (2 B)
 *   0x342A8800  g_packet_buf    (3074 B)
 * ─────────────────────────────────────────────────────────────────────────*/

/* USBX Memory Pool — muss DMA-zugaenglich sein (USBX-interne Transfer-Buffer) */
#define USBX_POOL_ADDR    0x342A0000U
static uint8_t * const usbx_mem_pool = (uint8_t *)USBX_POOL_ADDR;

/* Deskriptor-Puffer — werden von ux_device_stack_initialize() gelesen */
#define USB_DESC_HS_ADDR  0x342A8000U
#define USB_DESC_FS_ADDR  0x342A8200U
#define USB_STRINGS_ADDR  0x342A8400U
#define USB_LANGID_ADDR   0x342A8600U
#define PACKET_BUF_ADDR   0x342A8800U

static uint8_t * const usb_desc_hs    = (uint8_t *)USB_DESC_HS_ADDR;
static uint8_t * const usb_desc_fs    = (uint8_t *)USB_DESC_FS_ADDR;
static uint8_t * const usb_dev_strings = (uint8_t *)USB_STRINGS_ADDR;
static uint8_t * const usb_dev_langid  = (uint8_t *)USB_LANGID_ADDR;

/* UVC ISO Payload-Puffer */
static uint8_t * const g_packet_buf = (uint8_t *)PACKET_BUF_ADDR;

/* ─── USBX Task-Thread ─────────────────────────────────────────────────────
 * ST-Muster (FreeRTOS → Zephyr adaptiert):
 *   ISR: HAL_PCD_IRQHandler → k_sem_give → irq_disable(USB_IRQ)
 *   Task: k_sem_take(FOREVER) → tasks_run×2 → irq_enable(USB_IRQ)
 *
 * Vorteil: tasks_run() laeuft OHNE Race-Condition mit neuem USB IRQ.
 * IRQ wird erst nach tasks_run() re-enabled → serialisierte Verarbeitung.
 * Prio K_PRIO_COOP(0) = hoechste kooperative Prio → sofortiger Context-Switch
 * nach irq_disable im ISR. */
K_SEM_DEFINE(g_usb_event_sem, 0, 1);

#define USBX_TASK_STACK_SIZE  2048U
#define USBX_TASK_PRIO        K_PRIO_COOP(0)

K_THREAD_STACK_DEFINE(usbx_task_stack, USBX_TASK_STACK_SIZE);
static struct k_thread usbx_task_thread;

static void xi640_usbx_task_thread_fn(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;
    while (1) {
        k_sem_take(&g_usb_event_sem, K_FOREVER);
        ux_system_tasks_run();
        ux_system_tasks_run();
        irq_enable(XI640_DCD_IRQ_NUM);
    }
}

/* Colorbar Frame Buffer in AXISRAM3 — fest adressiert (kein BSS) */
#define FRAME_BUF_ADDR   0x34200000U
#define FRAME_WIDTH      640U
#define FRAME_HEIGHT     480U
#define FRAME_BUF_SIZE   (FRAME_WIDTH * FRAME_HEIGHT * 2U)  /* YUYV: 614400 Bytes */

#define RX_PACKET_SIZE   512U
static uint8_t rx_buffer[RX_PACKET_SIZE];

/* ── UVC Probe/Commit Fehler-Code ─────────────────────────────────────── */
static uint8_t g_error_code = UVC_NO_ERROR_ERR;

/* ─────────────────────────────────────────────────────────────────────── */
/*  YUYV Colorbar Generator                                                */
/* ─────────────────────────────────────────────────────────────────────── */

static void generate_colorbar(uint8_t *buf, uint32_t width, uint32_t height)
{
    /* 8 Farbbalkenwerte: {Y, U(Cb), V(Cr)} */
    static const uint8_t bars[8][3] = {
        {235, 128, 128},  /* Weiss   */
        {219,  16, 138},  /* Gelb    */
        {188, 154,   2},  /* Cyan    */
        {173,  42,  10},  /* Gruen   */
        { 78, 214, 246},  /* Magenta */
        { 63, 102, 254},  /* Rot     */
        { 32, 240, 118},  /* Blau    */
        { 16, 128, 128},  /* Schwarz */
    };

    uint32_t bar_w = width / 8U;

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = buf + y * (width * 2U);
        for (uint32_t mp = 0; mp < width / 2U; mp++) {
            uint32_t x   = mp * 2U;
            uint32_t bar = (x / bar_w);
            if (bar >= 8U) { bar = 7U; }
            row[mp * 4U + 0U] = bars[bar][0];  /* Y0 */
            row[mp * 4U + 1U] = bars[bar][1];  /* U  */
            row[mp * 4U + 2U] = bars[bar][0];  /* Y1 */
            row[mp * 4U + 3U] = bars[bar][2];  /* V  */
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  UVCL-kompatible Hilfsfunktionen (lokal implementiert — kein UVCL)     */
/* ─────────────────────────────────────────────────────────────────────── */

/**
 * @brief Neuen Frame-Transfer starten.
 *
 * Prueft ob Framerate-Periode abgelaufen ist und ein Frame bereit liegt.
 * Setzt on_fly_ctx auf den neuen Frame.
 *
 * @return Zeiger auf on_fly_ctx oder NULL wenn kein Frame bereit.
 */
uvc_on_fly_ctx *UVC_StartNewFrameTransmission(uvc_ctx_t *p_ctx, int packet_size)
{
    uint32_t now = HAL_GetTick();

    /* Warte auf naechste Frame-Periode (ausser bei is_starting) */
    if (!p_ctx->is_starting &&
        (int32_t)(now - p_ctx->frame_start) < p_ctx->frame_period_in_ms) {
        return NULL;
    }

    /* Kein Frame verfuegbar */
    if (!p_ctx->p_frame || !p_ctx->frame_size) {
        return NULL;
    }

    p_ctx->frame_start = now;
    p_ctx->is_starting = 0;

    /* Frame-ID toggeln (Bit 0 im BFH-Byte) */
    p_ctx->packet[1] ^= 0x01U;

    /* on_fly_ctx befuellen */
    uvc_on_fly_ctx *ctx = &p_ctx->on_fly_storage_ctx;
    int payload          = packet_size - 2;  /* UVC-Header abziehen */
    ctx->cursor          = p_ctx->p_frame;
    ctx->packet_nb       = (p_ctx->frame_size + payload - 1) / payload;
    ctx->last_packet_size = p_ctx->frame_size - (ctx->packet_nb - 1) * payload;
    ctx->packet_index    = 0;
    ctx->p_frame         = p_ctx->p_frame;

    p_ctx->on_fly_ctx = ctx;
    return ctx;
}

/**
 * @brief Cursor und Packet-Index nach gesendeter Payload vorruecken.
 */
void UVC_UpdateOnFlyCtx(uvc_ctx_t *p_ctx, int len)
{
    uvc_on_fly_ctx *ctx = p_ctx->on_fly_ctx;

    ctx->cursor       += (uint32_t)(len - 2);
    ctx->packet_index++;

    if (ctx->packet_index >= ctx->packet_nb) {
        /* Letztes Paket des Frames gesendet */
        p_ctx->on_fly_ctx = NULL;
    }
}

/**
 * @brief Laufenden Frame-Transfer abbrechen.
 */
void UVC_AbortOnFlyCtx(uvc_ctx_t *p_ctx)
{
    p_ctx->on_fly_ctx = NULL;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  UVC Setup-Request Handler (Probe/Commit Negotiation)                  */
/* ─────────────────────────────────────────────────────────────────────── */

static void probe_fill_defaults(uvc_ctx_t *p_ctx, uvc_VideoControlTypeDef *ctrl, int mps)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->bmHint                   = 0x0001U;  /* dwFrameInterval fixiert */
    ctrl->bFormatIndex             = 1U;
    ctrl->bFrameIndex              = 1U;
    ctrl->dwFrameInterval          = 333333U;  /* 100ns-Einheiten = 30 FPS */
    ctrl->dwMaxVideoFrameSize      = (uint32_t)(p_ctx->conf.width
                                                * p_ctx->conf.height * 2U);
    ctrl->dwMaxPayloadTransferSize = (uint32_t)mps;
    ctrl->dwClockFrequency         = 30000000U;
    ctrl->bmFramingInfo            = 3U;  /* FID + EOF im Payload-Header */
    ctrl->bPreferedVersion         = 1U;
    ctrl->bMinVersion              = 1U;
    ctrl->bMaxVersion              = 1U;
}

/**
 * @brief UVC Class-spezifische Setup-Requests bearbeiten.
 *
 * Implementiert Probe/Commit Negotiation nach UVC 1.5 Spec.
 * Ersetzt UVC_handle_setup_request() aus der ST UVCL-Bibliothek.
 */
int UVC_handle_setup_request(uvc_ctx_t *p_ctx, uvc_setup_req_t *req)
{
    uint8_t cs    = (uint8_t)(req->wValue >> 8);
    uint8_t iface = (uint8_t)(req->wIndex & 0xFFU);
    int     mps   = req->dwMaxPayloadTransferSize;
    int     len   = (int)req->wLength;

    if (len > (int)sizeof(uvc_VideoControlTypeDef)) {
        len = (int)sizeof(uvc_VideoControlTypeDef);
    }

    /* ── VideoControl Interface (IF 0): nur Error-Code-Anfragen ─────── */
    if (iface == 0U) {
        if (req->bRequest == UVC_GET_CUR &&
            cs == VC_REQUEST_ERROR_CODE_CONTROL) {
            return req->send_data(req->ctx, &g_error_code, 1);
        }
        g_error_code = UVC_INVALID_CONTROL_ERR;
        return -1;
    }

    /* ── VideoStreaming Interface (IF 1): Probe/Commit ──────────────── */
    switch (req->bRequest) {

    case UVC_SET_CUR:
        if (cs == VS_PROBE_CONTROL_CS) {
            req->receive_data(req->ctx,
                              (uint8_t *)&p_ctx->UVC_VideoProbeControl,
                              sizeof(uvc_VideoControlTypeDef));
            /* Kritische Werte erzwingen */
            p_ctx->UVC_VideoProbeControl.dwMaxPayloadTransferSize = (uint32_t)mps;
            p_ctx->UVC_VideoProbeControl.dwMaxVideoFrameSize =
                (uint32_t)(p_ctx->conf.width * p_ctx->conf.height * 2U);
            g_error_code = UVC_NO_ERROR_ERR;
            return 0;
        } else if (cs == VS_COMMIT_CONTROL_CS) {
            req->receive_data(req->ctx,
                              (uint8_t *)&p_ctx->UVC_VideoCommitControl,
                              sizeof(uvc_VideoControlTypeDef));
            g_error_code = UVC_NO_ERROR_ERR;
            return 0;
        }
        break;

    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_DEF:
        if (cs == VS_PROBE_CONTROL_CS) {
            probe_fill_defaults(p_ctx, &p_ctx->UVC_VideoProbeControl, mps);
            return req->send_data(req->ctx,
                                  (uint8_t *)&p_ctx->UVC_VideoProbeControl,
                                  len);
        } else if (cs == VS_COMMIT_CONTROL_CS) {
            probe_fill_defaults(p_ctx, &p_ctx->UVC_VideoCommitControl, mps);
            return req->send_data(req->ctx,
                                  (uint8_t *)&p_ctx->UVC_VideoCommitControl,
                                  len);
        }
        break;

    case UVC_GET_INFO:
        if (cs == VS_PROBE_CONTROL_CS || cs == VS_COMMIT_CONTROL_CS) {
            uint8_t info = UVC_SUPPORTS_GET | UVC_SUPPORTS_SET;
            return req->send_data(req->ctx, &info, 1);
        }
        break;

    case UVC_GET_LEN:
        if (cs == VS_PROBE_CONTROL_CS || cs == VS_COMMIT_CONTROL_CS) {
            uint16_t ctrl_len = (uint16_t)sizeof(uvc_VideoControlTypeDef);
            return req->send_data(req->ctx, (uint8_t *)&ctrl_len, 2);
        }
        break;
    }

    g_error_code = UVC_INVALID_CONTROL_ERR;
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  UVC Streaming Callbacks (1:1 aus ST usbx.c)                          */
/* ─────────────────────────────────────────────────────────────────────── */

#ifdef ISP_ENABLE_UVC

static int is_hs(void)
{
    assert(_ux_system_slave);
    return _ux_system_slave->ux_system_slave_speed == UX_HIGH_SPEED_DEVICE;
}

static uvc_ctx_t *UVC_usbx_get_ctx_from_video_instance(UX_DEVICE_CLASS_VIDEO *video_instance)
{
    return video_instance->ux_device_class_video_callbacks.ux_device_class_video_arg;
}

static uvc_ctx_t *UVC_usbx_get_ctx_from_stream(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream)
{
    return UVC_usbx_get_ctx_from_video_instance(stream->ux_device_class_video_stream_video);
}

static void UVC_SendPacket(uvc_ctx_t *p_ctx,
                           UX_DEVICE_CLASS_VIDEO_STREAM *stream, int len)
{
    ULONG  buffer_length;
    UCHAR *buffer;
    int    ret;

    ret = ux_device_class_video_write_payload_get(stream, &buffer, &buffer_length);
    assert(ret == UX_SUCCESS);
    assert((int)buffer_length >= len);

    memcpy(buffer, p_ctx->packet, (size_t)len);
    ret = ux_device_class_video_write_payload_commit(stream, (ULONG)len);
    assert(ret == UX_SUCCESS);
}

static void UVC_DataIn(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream)
{
    int packet_size = is_hs() ? UVC_ISO_HS_MPS : UVC_ISO_FS_MPS;
    uvc_ctx_t    *p_ctx    = UVC_usbx_get_ctx_from_stream(stream);
    uvc_on_fly_ctx *on_fly_ctx;
    int len;

    if (p_ctx->state != UVC_STATUS_STREAMING) {
        return;
    }

    /* Neuen Frame starten wenn keiner laeuft */
    if (!p_ctx->on_fly_ctx) {
        p_ctx->on_fly_ctx = UVC_StartNewFrameTransmission(p_ctx, packet_size);
    }

    if (!p_ctx->on_fly_ctx) {
        /* Kein Frame bereit — leeres Headerpaket senden */
        UVC_SendPacket(p_ctx, stream, 2);
        return;
    }

    /* Naechstes Paket senden */
    on_fly_ctx = p_ctx->on_fly_ctx;
    len = (on_fly_ctx->packet_index == (on_fly_ctx->packet_nb - 1))
          ? (on_fly_ctx->last_packet_size + 2)
          : packet_size;

    memcpy(&p_ctx->packet[2], on_fly_ctx->cursor, (size_t)(len - 2));
    UVC_SendPacket(p_ctx, stream, len);

    UVC_UpdateOnFlyCtx(p_ctx, len);
}

static void UVC_StopStreaming(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream)
{
    uvc_ctx_t *p_ctx = UVC_usbx_get_ctx_from_stream(stream);

    p_ctx->state = UVC_STATUS_STOP;
    if (p_ctx->on_fly_ctx) {
        UVC_AbortOnFlyCtx(p_ctx);
    }

    p_ctx->p_frame    = NULL;
    p_ctx->frame_size = 0;
    p_ctx->is_starting = 0;
}

static void UVC_StartStreaming(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream)
{
    uvc_ctx_t *p_ctx = UVC_usbx_get_ctx_from_stream(stream);
    int i;

    if (!p_ctx) {
        return;
    }

    /* UVC Payload-Header initialisieren */
    p_ctx->packet[0] = 2U;  /* Header-Laenge */
    p_ctx->packet[1] = 0U;  /* BFH: FID=0, alle anderen Bits 0 */

    /* Frame-Buffer fuer Colorbar setzen */
    p_ctx->p_frame    = (uint8_t *)FRAME_BUF_ADDR;
    p_ctx->frame_size = (int)FRAME_BUF_SIZE;

    /* Timing: sofort starten */
    p_ctx->frame_start = HAL_GetTick() - (uint32_t)p_ctx->frame_period_in_ms;
    p_ctx->is_starting  = 1;
    p_ctx->state        = UVC_STATUS_STREAMING;

    /* Alle Payload-Slots vorabfuellen */
    for (i = 0; i < p_ctx->buffer_nb; i++) {
        UVC_DataIn(stream);
    }
}

static void UVC_stream_change(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream,
                              ULONG alternate_setting)
{
    int ret;

    LOG_INF("UVC Alt-Setting: %lu", alternate_setting);

    if (alternate_setting == 0U) {
        UVC_StopStreaming(stream);
        return;
    }

    UVC_StartStreaming(stream);

    ret = ux_device_class_video_transmission_start(stream);
    assert(ret == UX_SUCCESS);
}

static int UVC_usbd_stop_streaming(void *ctx)
{
    (void)ctx;
    assert(0);  /* Wird im USBX-Pfad nicht aufgerufen */
    return -1;
}

static int UVC_usbd_start_streaming(void *ctx)
{
    (void)ctx;
    assert(0);  /* Wird im USBX-Pfad nicht aufgerufen */
    return -1;
}

static int UVC_usbx_send_data(void *ctx, uint8_t *data, int length)
{
    UX_SLAVE_TRANSFER *transfer = ctx;
    uint8_t *buffer = transfer->ux_slave_transfer_request_data_pointer;

    memcpy(buffer, data, (size_t)length);
    return (int)ux_device_stack_transfer_request(transfer,
                                                  (ULONG)length, (ULONG)length);
}

static int UVC_usbx_receive_data(void *ctx, uint8_t *data, int length)
{
    UX_SLAVE_TRANSFER *transfer = ctx;

    if ((int)transfer->ux_slave_transfer_request_actual_length != length) {
        return (int)UX_ERROR;
    }
    memcpy(data, transfer->ux_slave_transfer_request_data_pointer, (size_t)length);
    return (int)UX_SUCCESS;
}

static UINT UVC_stream_request(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream,
                               UX_SLAVE_TRANSFER *transfer)
{
    uvc_ctx_t    *p_ctx = UVC_usbx_get_ctx_from_stream(stream);
    uvc_setup_req_t req;

    req.bmRequestType = transfer->ux_slave_transfer_request_setup[UX_SETUP_REQUEST_TYPE];
    req.bRequest      = transfer->ux_slave_transfer_request_setup[UX_SETUP_REQUEST];
    req.wValue        = ux_utility_short_get(transfer->ux_slave_transfer_request_setup
                                             + UX_SETUP_VALUE);
    req.wIndex        = ux_utility_short_get(transfer->ux_slave_transfer_request_setup
                                             + UX_SETUP_INDEX);
    req.wLength       = ux_utility_short_get(transfer->ux_slave_transfer_request_setup
                                             + UX_SETUP_LENGTH);
    req.dwMaxPayloadTransferSize = is_hs() ? UVC_ISO_HS_MPS : UVC_ISO_FS_MPS;
    req.ctx              = transfer;
    req.stop_streaming   = UVC_usbd_stop_streaming;
    req.start_streaming  = UVC_usbd_start_streaming;
    req.send_data        = UVC_usbx_send_data;
    req.receive_data     = UVC_usbx_receive_data;

    return UVC_handle_setup_request(p_ctx, &req) ? UX_ERROR : UX_SUCCESS;
}

static VOID UVC_stream_payload_done(struct UX_DEVICE_CLASS_VIDEO_STREAM_STRUCT *stream,
                                    ULONG length)
{
    (void)length;
    UVC_DataIn(stream);
}

static VOID UVC_instance_activate(VOID *video_instance)
{
    uvc_ctx_t *p_ctx = UVC_usbx_get_ctx_from_video_instance(video_instance);
    p_ctx->state = UVC_STATUS_STOP;
    LOG_INF("UVC Instance aktiviert");
}

static VOID UVC_instance_deactivate(VOID *video_instance)
{
    (void)video_instance;
    LOG_INF("UVC Instance deaktiviert");
}

#endif /* ISP_ENABLE_UVC */

/* ─────────────────────────────────────────────────────────────────────── */
/*  USBX Device Callback: tasks_run() aus SOF-ISR                        */
/* ─────────────────────────────────────────────────────────────────────── */

static UINT usbx_device_cb(ULONG cb_evt)
{
    /* tasks_run() wird vom Task-Thread via g_usb_event_sem getrieben.
     * SOF-Callback wird nicht mehr verwendet — vermeidet ISR-Ueberlastung
     * bei RXFLVL-Flut im PIO-Modus. */
    (void)cb_evt;
    return 0U;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  String-Deskriptor Hilfsfunktionen (1:1 aus ST usbx.c)                */
/* ─────────────────────────────────────────────────────────────────────── */

static int usbx_extract_string(uint8_t langid[2], int index,
                                uint8_t *string_desc,
                                uint8_t *p_dst, int dst_len)
{
    int str_len = ((int)string_desc[0] - 2) / 2;
    int i;

    if (dst_len < str_len + 4) {
        return -1;
    }

    p_dst[0] = langid[0];
    p_dst[1] = langid[1];
    p_dst[2] = (uint8_t)index;
    p_dst[3] = (uint8_t)str_len;
    for (i = 0; i < str_len; i++) {
        p_dst[4 + i] = string_desc[2 + 2 * i];
    }
    return str_len + 4;
}

static int usbx_build_dev_strings(uint8_t langid[2],
                                   uint8_t *p_dst, int dst_len)
{
    uint8_t string_desc[128];
    int res = 0;
    int len;

    len = usb_get_manufacturer_string_desc(string_desc, (int)sizeof(string_desc));
    if (len < 0) { return len; }
    res += usbx_extract_string(langid, 1, string_desc, &p_dst[res], dst_len - res);
    if (res < 0) { return 0; }

    len = usb_get_product_string_desc(string_desc, (int)sizeof(string_desc));
    if (len < 0) { return len; }
    res += usbx_extract_string(langid, 2, string_desc, &p_dst[res], dst_len - res);
    if (res < 0) { return 0; }

    len = usb_get_serial_string_desc(string_desc, (int)sizeof(string_desc));
    if (len < 0) { return len; }
    res += usbx_extract_string(langid, 3, string_desc, &p_dst[res], dst_len - res);
    if (res < 0) { return 0; }

    return res;
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  CDC ACM Callbacks                                                      */
/* ─────────────────────────────────────────────────────────────────────── */

static UX_SLAVE_CLASS_CDC_ACM *cdc_acm;

VOID USBD_CDC_ACM_Activate(VOID *cdc_acm_instance)
{
    LOG_INF("CDC ACM aktiviert: %p", cdc_acm_instance);
    cdc_acm = cdc_acm_instance;
}

VOID USBD_CDC_ACM_Deactivate(VOID *cdc_acm_instance)
{
    LOG_INF("CDC ACM deaktiviert: %p", cdc_acm_instance);
    cdc_acm = NULL;
}

VOID USBD_CDC_ACM_ParameterChange(VOID *cdc_acm_instance)
{
    UX_SLAVE_CLASS_CDC_ACM_LINE_CODING_PARAMETER line_coding;
    UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_PARAMETER  line_state;
    UX_SLAVE_TRANSFER *transfer_request;
    UX_SLAVE_DEVICE   *device;
    ULONG request;
    int ret;

    device           = &_ux_system_slave->ux_system_slave_device;
    transfer_request = &device->ux_slave_device_control_endpoint
                           .ux_slave_endpoint_transfer_request;
    request = *(transfer_request->ux_slave_transfer_request_setup
                + UX_SETUP_REQUEST);

    switch (request) {
    case UX_SLAVE_CLASS_CDC_ACM_SET_LINE_CODING:
        ret = ux_device_class_cdc_acm_ioctl(cdc_acm_instance,
                  UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_CODING, &line_coding);
        assert(ret == 0);
        LOG_INF("CDC Set %d bps",
                (int)line_coding.ux_slave_class_cdc_acm_parameter_baudrate);
        break;
    case UX_SLAVE_CLASS_CDC_ACM_SET_CONTROL_LINE_STATE:
        ret = ux_device_class_cdc_acm_ioctl(cdc_acm_instance,
                  UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_STATE, &line_state);
        assert(ret == 0);
        LOG_INF("CDC Line rts=%d dtr=%d",
                (int)line_state.ux_slave_class_cdc_acm_parameter_rts,
                (int)line_state.ux_slave_class_cdc_acm_parameter_dtr);
        break;
    default:
        LOG_WRN("CDC unbekannte Anfrage 0x%lx", request);
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  CDC Read/Write API (optional, fuer spaetere Nutzung)                 */
/* ─────────────────────────────────────────────────────────────────────── */

uint32_t usbx_read(uint8_t *payload)
{
    UX_SLAVE_DEVICE *device = &_ux_system_slave->ux_system_slave_device;
    ULONG rx_len = 0;
    int ret;

    if (device->ux_slave_device_state != UX_DEVICE_CONFIGURED) { return 0U; }
    if (!cdc_acm) { return 0U; }

    ret = ux_device_class_cdc_acm_read_run(cdc_acm, rx_buffer,
                                            RX_PACKET_SIZE, &rx_len);
    if (ret == UX_STATE_NEXT && rx_len > 0U) {
        memcpy(payload, rx_buffer, rx_len);
        return (uint32_t)rx_len;
    }
    return 0U;
}

void usbx_write(uint8_t *msg, uint32_t len)
{
    UX_SLAVE_DEVICE *device = &_ux_system_slave->ux_system_slave_device;
    UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_PARAMETER line_state;
    ULONG len_send;
    int ret;

    if (device->ux_slave_device_state != UX_DEVICE_CONFIGURED) { return; }
    if (!cdc_acm) { return; }

    ret = ux_device_class_cdc_acm_ioctl(cdc_acm,
              UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_STATE, &line_state);
    assert(ret == 0);
    if (!line_state.ux_slave_class_cdc_acm_parameter_dtr) { return; }

    ret = ux_device_class_cdc_acm_write_run(cdc_acm, msg, len, &len_send);
    while (ret == UX_STATE_WAIT) {
        ret = ux_device_class_cdc_acm_write_run(cdc_acm, msg, len, &len_send);
    }
    assert(ret == UX_STATE_NEXT);
}

/* ─────────────────────────────────────────────────────────────────────── */
/*  usbx_init() — USBX System + Device Stack + UVC + CDC initialisieren  */
/* ─────────────────────────────────────────────────────────────────────── */

/**
 * @brief USBX vollstaendig initialisieren.
 *
 * Ruft ux_system_initialize(), baut Deskriptoren, ruft
 * ux_device_stack_initialize(), registriert UVC (IF 0) und CDC (IF 2),
 * ruft abschliessend xi640_dcd_register_usbx() (= _ux_dcd_stm32_initialize).
 *
 * Muss nach xi640_dcd_init() und vor xi640_dcd_start() aufgerufen werden.
 *
 * @param p_ctx  Zeiger auf uvc_ctx_t mit conf.width/height/fps befuellt.
 * @return 0 bei Erfolg, negativer Fehlercode bei Fehler.
 */
int usbx_init(uvc_ctx_t *p_ctx)
{
    uint8_t lang_string_desc[4];
    int usb_dev_strings_len;
    int usb_dev_langid_len;
    int usb_desc_hs_len;
    int usb_desc_fs_len;
    int len;
    int ret;

    /* ── AXISRAM-Puffer nullen (kein BSS-Zero-Init bei festen Adressen) ── */
    memset((void *)USBX_POOL_ADDR,   0, USBX_MEM_SIZE);
    memset((void *)USB_DESC_HS_ADDR, 0, USBX_MAX_CONF_LEN * 2U + USBX_MAX_STRING_LEN + 8U
                                        + (UVC_ISO_HS_MPS + 2U));

    /* ── Colorbar Testbild generieren ─────────────────────────────────── */
    LOG_INF("Generiere YUYV Colorbar (%lux%lu)...", FRAME_WIDTH, FRAME_HEIGHT);
    generate_colorbar((uint8_t *)FRAME_BUF_ADDR, FRAME_WIDTH, FRAME_HEIGHT);
    LOG_INF("Colorbar bereit (Adresse 0x%08x, %lu Bytes)",
            FRAME_BUF_ADDR, FRAME_BUF_SIZE);

    /* ── uvc_ctx_t Felder befuellen ────────────────────────────────────── */
    p_ctx->packet            = g_packet_buf;
    p_ctx->buffer_nb         = (int)USBX_BUFFER_NB;
    p_ctx->frame_period_in_ms = (p_ctx->conf.fps > 0)
                                 ? (1000 / (int)p_ctx->conf.fps) : 33;

    /* ── USBX System initialisieren ────────────────────────────────────── */
    ret = (int)ux_system_initialize(usbx_mem_pool, USBX_MEM_SIZE,
                                    UX_NULL, 0U);
    if (ret) {
        LOG_ERR("ux_system_initialize fehlgeschlagen: 0x%x", ret);
        return ret;
    }
    LOG_INF("USBX System initialisiert (%u KB Pool)", USBX_MEM_SIZE / 1024U);

#ifdef ISP_ENABLE_UVC
    {
        uvc_desc_conf desc_conf_uvc = { 0 };
        desc_conf_uvc.width  = p_ctx->conf.width;
        desc_conf_uvc.height = p_ctx->conf.height;
        desc_conf_uvc.fps    = p_ctx->conf.fps;

        /* HS Deskriptor */
        desc_conf_uvc.is_hs = 1;
        usb_desc_hs_len = uvc_get_device_desc(usb_desc_hs,
                                               (int)USBX_MAX_CONF_LEN,
                                               1, 2, 3);
        assert(usb_desc_hs_len > 0);

        len = uvc_get_configuration_desc(&usb_desc_hs[usb_desc_hs_len],
                                          (int)USBX_MAX_CONF_LEN - usb_desc_hs_len,
                                          &desc_conf_uvc);
        assert(len > 0);
        usb_desc_hs_len += len;

        /* FS Deskriptor */
        desc_conf_uvc.is_hs = 0;
        usb_desc_fs_len = uvc_get_device_desc(usb_desc_fs,
                                               (int)USBX_MAX_CONF_LEN,
                                               1, 2, 3);
        assert(usb_desc_fs_len > 0);

        len = uvc_get_configuration_desc(&usb_desc_fs[usb_desc_fs_len],
                                          (int)USBX_MAX_CONF_LEN - usb_desc_fs_len,
                                          &desc_conf_uvc);
        assert(len > 0);
        usb_desc_fs_len += len;
    }
#else
    {
        usb_desc_conf desc_conf_usb = { 0 };
        desc_conf_usb.is_hs = 1;
        usb_desc_hs_len = usb_get_device_desc(usb_desc_hs,
                                               (int)USBX_MAX_CONF_LEN,
                                               1, 2, 3);
        assert(usb_desc_hs_len > 0);
        len = usb_get_configuration_desc(&usb_desc_hs[usb_desc_hs_len],
                                          (int)USBX_MAX_CONF_LEN - usb_desc_hs_len,
                                          &desc_conf_usb);
        assert(len > 0);
        usb_desc_hs_len += len;

        desc_conf_usb.is_hs = 0;
        usb_desc_fs_len = usb_get_device_desc(usb_desc_fs,
                                               (int)USBX_MAX_CONF_LEN,
                                               1, 2, 3);
        assert(usb_desc_fs_len > 0);
        len = usb_get_configuration_desc(&usb_desc_fs[usb_desc_fs_len],
                                          (int)USBX_MAX_CONF_LEN - usb_desc_fs_len,
                                          &desc_conf_usb);
        assert(len > 0);
        usb_desc_fs_len += len;
    }
#endif /* ISP_ENABLE_UVC */

    LOG_INF("Deskriptoren: HS=%d Bytes, FS=%d Bytes",
            usb_desc_hs_len, usb_desc_fs_len);

    /* ── String-Deskriptoren ───────────────────────────────────────────── */
    len = usb_get_lang_string_desc(lang_string_desc, (int)sizeof(lang_string_desc));
    assert(len == (int)sizeof(lang_string_desc));
    usb_dev_langid[0]  = lang_string_desc[2];
    usb_dev_langid[1]  = lang_string_desc[3];
    usb_dev_langid_len = 2;

    usb_dev_strings_len = usbx_build_dev_strings(usb_dev_langid,
                                                   usb_dev_strings,
                                                   (int)USBX_MAX_STRING_LEN);
    assert(usb_dev_strings_len > 0);

    /* ── Device Stack initialisieren ───────────────────────────────────── */
    ret = (int)ux_device_stack_initialize(
              usb_desc_hs, (ULONG)usb_desc_hs_len,
              usb_desc_fs, (ULONG)usb_desc_fs_len,
              usb_dev_strings, (ULONG)usb_dev_strings_len,
              usb_dev_langid, (ULONG)usb_dev_langid_len,
              usbx_device_cb);
    if (ret) {
        LOG_ERR("ux_device_stack_initialize fehlgeschlagen: 0x%x", ret);
        return ret;
    }
    LOG_INF("USBX Device Stack initialisiert");

    /* ── CDC ACM Parameter ─────────────────────────────────────────────── */
    UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_acm_parameter;
    cdc_acm_parameter.ux_slave_class_cdc_acm_instance_activate   = USBD_CDC_ACM_Activate;
    cdc_acm_parameter.ux_slave_class_cdc_acm_instance_deactivate = USBD_CDC_ACM_Deactivate;
    cdc_acm_parameter.ux_slave_class_cdc_acm_parameter_change    = USBD_CDC_ACM_ParameterChange;

#ifdef ISP_ENABLE_UVC
    {
        UX_DEVICE_CLASS_VIDEO_STREAM_PARAMETER vsp[1] = { 0 };
        UX_DEVICE_CLASS_VIDEO_PARAMETER        vp     = { 0 };

#if defined(UX_DEVICE_STANDALONE)
        vsp[0].ux_device_class_video_stream_parameter_task_function =
            ux_device_class_video_write_task_function;
#else
        vsp[0].ux_device_class_video_stream_parameter_thread_stack_size = 0U;
        vsp[0].ux_device_class_video_stream_parameter_thread_entry =
            ux_device_class_video_write_thread_entry;
#endif
        vsp[0].ux_device_class_video_stream_parameter_callbacks
            .ux_device_class_video_stream_change        = UVC_stream_change;
        vsp[0].ux_device_class_video_stream_parameter_callbacks
            .ux_device_class_video_stream_request       = UVC_stream_request;
        vsp[0].ux_device_class_video_stream_parameter_callbacks
            .ux_device_class_video_stream_payload_done  = UVC_stream_payload_done;
        vsp[0].ux_device_class_video_stream_parameter_max_payload_buffer_nb   = USBX_BUFFER_NB;
        vsp[0].ux_device_class_video_stream_parameter_max_payload_buffer_size = UVC_ISO_HS_MPS;

        vp.ux_device_class_video_parameter_callbacks
            .ux_slave_class_video_instance_activate   = UVC_instance_activate;
        vp.ux_device_class_video_parameter_callbacks
            .ux_slave_class_video_instance_deactivate = UVC_instance_deactivate;
        vp.ux_device_class_video_parameter_callbacks
            .ux_device_class_video_request            = NULL;
        vp.ux_device_class_video_parameter_callbacks
            .ux_device_class_video_arg                = p_ctx;
        vp.ux_device_class_video_parameter_streams_nb = 1U;
        vp.ux_device_class_video_parameter_streams    = vsp;

        /* Video-Klasse auf Interface 0 registrieren */
        ret = (int)ux_device_stack_class_register(
                  _ux_system_device_class_video_name,
                  ux_device_class_video_entry, 1U, 0U, &vp);
        if (ret) {
            LOG_ERR("Video-Klasse Registrierung fehlgeschlagen: 0x%x", ret);
            return ret;
        }
        LOG_INF("UVC Video-Klasse registriert (IF 0)");

        /* CDC-Klasse auf Interface 2 registrieren */
        ret = (int)ux_device_stack_class_register(
                  _ux_system_slave_class_cdc_acm_name,
                  ux_device_class_cdc_acm_entry, 1U, 2U, &cdc_acm_parameter);
        assert(ret == 0);
        LOG_INF("CDC ACM registriert (IF 2)");
    }
#else
    /* Nur CDC: Interface 0 */
    ret = (int)ux_device_stack_class_register(
              _ux_system_slave_class_cdc_acm_name,
              ux_device_class_cdc_acm_entry, 1U, 0U, &cdc_acm_parameter);
    assert(ret == 0);
    LOG_INF("CDC ACM registriert (IF 0, nur-CDC Modus)");
#endif /* ISP_ENABLE_UVC */

    /* ── USBX Task-Thread starten (vor DCD-Registrierung) ──────────────── */
    k_thread_create(&usbx_task_thread,
                    usbx_task_stack, K_THREAD_STACK_SIZEOF(usbx_task_stack),
                    xi640_usbx_task_thread_fn, NULL, NULL, NULL,
                    USBX_TASK_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&usbx_task_thread, "usbx_tasks");
    LOG_INF("USBX Task-Thread gestartet (Prio %d, Stack %u B)",
            USBX_TASK_PRIO, USBX_TASK_STACK_SIZE);

    /* ── DCD registrieren (= _ux_dcd_stm32_initialize) ────────────────── */
    xi640_dcd_status_t dcd_ret = xi640_dcd_register_usbx();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_register_usbx fehlgeschlagen: %d", dcd_ret);
        return -1;
    }

    LOG_INF("usbx_init abgeschlossen");
    return 0;
}

/* Hinweis: _ux_utility_interrupt_disable/restore() sind in ux_port.c
 * implementiert (app/usbx_zephyr_port/src/ux_port.c). Keine Duplikate hier. */
