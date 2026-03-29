/**
 * @file    xi640_uvc_descriptors.h
 * @brief   USB/UVC Deskriptoren fuer Xi640 UVC Kamera
 *
 * Deskriptor-Bloecke fuer ux_device_stack_initialize().
 * Werden durch xi640_uvc_descriptors_build() zur Laufzeit befuellt —
 * exakt nach ST-Referenz (usb_desc.c + uvcl_desc.c, x-cube-n6-camera-capture).
 *
 * Unterstuetzte Konfiguration:
 *   - Video Class 1.5 (bcdUVC = 0x0150)
 *   - Format: YUYV (uncompressed, 640x480 @ 30 FPS)
 *   - Endpoint: EP1 IN ISO High-Bandwidth (3x1024 = 3072 Bytes/Microframe)
 *   - idVendor = 0x0483, idProduct = 0x5740 (ST UVC)
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#ifndef XI640_UVC_DESCRIPTORS_H
#define XI640_UVC_DESCRIPTORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HS Framework (Device Desc + Config + alle Sub-Descs) ────────────── */
extern uint8_t  xi640_uvc_hs_framework[512];
extern uint32_t xi640_uvc_hs_framework_len;

/* ── FS Framework ─────────────────────────────────────────────────────── */
extern uint8_t  xi640_uvc_fs_framework[512];
extern uint32_t xi640_uvc_fs_framework_len;

/* ── String Framework ────────────────────────────────────────────────── */
/* Format pro Eintrag: [lang_lo][lang_hi][string_index][len][chars...]   */
extern uint8_t  xi640_uvc_string_framework[128];
extern uint32_t xi640_uvc_string_framework_len;

/* ── Language ID Framework ───────────────────────────────────────────── */
extern uint8_t  xi640_uvc_language_id_framework[2];
extern uint32_t xi640_uvc_language_id_framework_len;

/**
 * @brief Deskriptor-Puffer befuellen (ST-Referenz-Logik).
 *
 * Muss VOR ux_device_stack_initialize() aufgerufen werden.
 * Generiert Serial-Nummer aus STM32 UID (HAL_GetUIDw0/1/2).
 */
void xi640_uvc_descriptors_build(void);

/* ── Getter-Funktionen ───────────────────────────────────────────────── */
const uint8_t *xi640_uvc_get_hs_framework(void);
uint32_t       xi640_uvc_get_hs_framework_length(void);
const uint8_t *xi640_uvc_get_fs_framework(void);
uint32_t       xi640_uvc_get_fs_framework_length(void);

#ifdef __cplusplus
}
#endif

#endif /* XI640_UVC_DESCRIPTORS_H */
