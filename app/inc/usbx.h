/**
 * @file    usbx.h
 * @brief   USBX Init-API fuer Xi640 — basiert auf ST-Referenz usbx.h
 *
 * Angepasste Signatur: PCD-Handle wird intern ueber xi640_dcd_get_pcd_handle()
 * geholt — main.c benoetigt keinen direkten HAL-Typ.
 *
 * Copyright (c) 2025 STMicroelectronics / 2026 Optris GmbH.
 */

#ifndef _USBX_H_
#define _USBX_H_

#include "usbx_conf.h"

/**
 * @brief  USBX System + Device Stack + UVC/CDC Class initialisieren.
 *
 * Entspricht ST usbx_init() aber ohne externen PCD-Handle-Parameter.
 * Baut Deskriptoren (usb_desc.c), ruft ux_system_initialize(),
 * ux_device_stack_initialize(), registriert UVC + CDC, ruft
 * xi640_dcd_register_usbx() (= ux_dcd_stm32_initialize).
 *
 * @param  p_ctx  Zeiger auf befuellten uvc_ctx_t (conf.width/height/fps)
 * @return 0 bei Erfolg, negativer Fehlercode sonst
 */
int usbx_init(uvc_ctx_t *p_ctx);

#endif /* _USBX_H_ */
