/**
 * @file    xi640_st_test.h
 * @brief   ST-Referenzpfad Klon — Interface
 *
 * Minimales Testprogramm das EXAKT den ST-Initialisierungspfad aus
 * x-cube-n6-camera-capture nachbaut, in unserem Zephyr-Projekt.
 *
 * Ziel: Wenn dieses File enumeriert (STP>0, Windows zeigt UVC-Geraet),
 *       ist unser Glue-Code/Deskriptoren das Problem — wir arbeiten uns
 *       dann rueckwaerts zu unserem eigenen Code vor.
 *
 * Kritischer Unterschied zu unserem Code:
 *   ST: ux_system_tasks_run() ZWEIMAL direkt aus ISR (UVCL_stm32_usbx_IRQHandler)
 *   Wir: ux_system_tasks_run() aus dedizierten Thread mit k_sleep(1ms)
 *
 * Aktivierung: XI640_ST_ISR_MODE=1 in CMakeLists.txt
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#ifndef XI640_ST_TEST_H
#define XI640_ST_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   ST-Klon Initialisierung — ersetzt die 5-Schritt-Sequenz in main.c.
 *
 * Entspricht dem ST-Referenzpfad:
 *   1. UVCL_usb_init()  → xi640_dcd_init()  (HAL PCD + FIFO, identisch)
 *   2. usbx_init()      → xi640_uvc_init()  (USBX System + Stack + Video)
 *   3. ux_dcd_stm32_initialize() → xi640_dcd_register_usbx()
 *   4. (KEIN tasks-Thread — tasks_run() laeuft aus ISR via XI640_ST_ISR_MODE)
 *   5. HAL_PCD_Start()  → xi640_dcd_start() (IRQ enable + Enumeration)
 *
 * @return  0 bei Erfolg, negativer Fehlercode sonst
 */
int xi640_st_init(void);

#ifdef __cplusplus
}
#endif

#endif /* XI640_ST_TEST_H */
