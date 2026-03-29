/**
 * @file    usbx_conf.h
 * @brief   USBX Konfiguration fuer Xi640 — ST-Referenz nachempfunden
 *
 * Stellt alle Defines bereit die usb_desc.c und usbx_zephyr.c benoetigen.
 */

#ifndef USBX_CONF_H
#define USBX_CONF_H

#include <stdint.h>

/* ── Speicherpool ──────────────────────────────────────────────────────── */
#define USBX_MEM_SIZE         (32U * 1024U)   /* 32 KB (ST-Referenzwert) */
#define USBX_BUFFER_NB        4U               /* Payload-Buffer-Slots */

/* ── Deskriptor-Puffer ─────────────────────────────────────────────────── */
#define USBX_MAX_CONF_LEN     512U
#define USBX_MAX_STRING_LEN   512U
#define USBX_MAX_LANGID_LEN   2U

/* ── Alignment / Attribute ─────────────────────────────────────────────── */
#define USBX_ALIGN_32         __attribute__((aligned(32)))
#define USBX_UNCACHED         /* leer — DCache ist global deaktiviert */

/* ── ISO High-Bandwidth: 3 Transactions x 1024 = 3072 B/Microframe ─────── */
#ifndef USBL_PACKET_PER_MICRO_FRAME
#define USBL_PACKET_PER_MICRO_FRAME   3U
#endif

#define UVC_ISO_HS_MPS  (USBL_PACKET_PER_MICRO_FRAME * 1024U)
#define UVC_ISO_FS_MPS  1023U

/* ── UVC aktivieren ────────────────────────────────────────────────────── */
#define ISP_ENABLE_UVC

/* ── UVC Typen und Prototypen (aus ST usb_uvc.h) ──────────────────────── */
#include "usb_uvc.h"

/* Prototypen fuer UVCL-kompatible Funktionen (in usbx_zephyr.c) */
uvc_on_fly_ctx *UVC_StartNewFrameTransmission(uvc_ctx_t *p_ctx, int packet_size);
void            UVC_UpdateOnFlyCtx(uvc_ctx_t *p_ctx, int len);
void            UVC_AbortOnFlyCtx(uvc_ctx_t *p_ctx);
int             UVC_handle_setup_request(uvc_ctx_t *p_ctx, uvc_setup_req_t *req);

#endif /* USBX_CONF_H */
