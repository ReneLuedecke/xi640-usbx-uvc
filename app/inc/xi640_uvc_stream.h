/**
 * @file    xi640_uvc_stream.h
 * @brief   Xi640 UVC Colorbar Streaming — Interface
 *
 * Kapselt:
 *   - USBX System + Device Stack Initialisierung
 *   - UVC Video Class Registrierung
 *   - Colorbar-Testbild Erzeugung (YUYV, 640x480)
 *   - Streaming-Thread (Prio 2, 4096 Byte Stack)
 *   - ux_system_tasks_run() Pumpe fuer Enumeration + Streaming
 *
 * Initialisierungsreihenfolge (wird von main() aufgerufen):
 *   1. xi640_uvc_init()             — USBX + Video Class Setup
 *   2. xi640_dcd_register_usbx()   — DCD bei USBX registrieren
 *   3. xi640_uvc_stream_start()    — Streaming-Thread starten
 *   4. xi640_dcd_start()           — USB-Verbindung aktivieren
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#ifndef XI640_UVC_STREAM_H
#define XI640_UVC_STREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────── */
/*  Rueckgabewerte                                                         */
/* ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    XI640_UVC_OK                  = 0,
    XI640_UVC_ERR_SYSTEM_INIT     = -1,   /**< ux_system_initialize fehlgeschlagen */
    XI640_UVC_ERR_STACK_INIT      = -2,   /**< ux_device_stack_initialize fehler   */
    XI640_UVC_ERR_CLASS_REGISTER  = -3,   /**< ux_device_stack_class_register fehler */
} xi640_uvc_status_t;

/* ──────────────────────────────────────────────────────────────────────── */
/*  Oeffentliche API                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief   Initialisiert USBX System, Device Stack und Video Class.
 *
 * Muss NACH xi640_dcd_init() und VOR xi640_dcd_register_usbx() aufgerufen
 * werden.
 *
 * @return  XI640_UVC_OK bei Erfolg, Fehlercode sonst
 */
xi640_uvc_status_t xi640_uvc_init(void);

/**
 * @brief   Startet den UVC Streaming-Thread.
 *
 * Der Thread pumpt ux_system_tasks_run() (fuer Enumeration) und
 * sendet Colorbar-Payloads wenn der Host Streaming aktiviert hat.
 *
 * Muss VOR xi640_dcd_start() aufgerufen werden, damit Enumeration
 * verarbeitet werden kann.
 */
void xi640_uvc_stream_start(void);

#ifdef __cplusplus
}
#endif

#endif /* XI640_UVC_STREAM_H */
