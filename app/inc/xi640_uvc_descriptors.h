/**
 * @file    xi640_uvc_descriptors.h
 * @brief   USB/UVC Deskriptoren fuer Xi640 UVC Kamera
 *
 * Statische Deskriptor-Bloecke fuer ux_device_stack_initialize().
 * Format: USBX device_framework = Device Desc + Config Desc + Sub-Descs.
 *
 * Unterstuetzte Konfiguration:
 *   - Video Class 1.1 (bcdUVC = 0x0110)
 *   - Format: YUYV (uncompressed, 640x480 @ 30 FPS)
 *   - Endpoint: EP1 IN ISO High-Bandwidth (3x1024 = 3072 Bytes/Microframe)
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
extern const uint8_t  xi640_uvc_hs_framework[];
extern const uint32_t xi640_uvc_hs_framework_len;

/* ── FS Framework (identisch zu HS fuer Phase 5) ─────────────────────── */
extern const uint8_t  xi640_uvc_fs_framework[];
extern const uint32_t xi640_uvc_fs_framework_len;

/* ── String Framework ────────────────────────────────────────────────── */
/* Format pro Eintrag: [lang_lo][lang_hi][string_index][len][chars...]   */
extern const uint8_t  xi640_uvc_string_framework[];
extern const uint32_t xi640_uvc_string_framework_len;

/* ── Language ID Framework ───────────────────────────────────────────── */
extern const uint8_t  xi640_uvc_language_id_framework[];
extern const uint32_t xi640_uvc_language_id_framework_len;

#ifdef __cplusplus
}
#endif

#endif /* XI640_UVC_DESCRIPTORS_H */
