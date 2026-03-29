/**
 * @file    xi640_st_test.c
 * @brief   ST-Referenzpfad Klon fuer Zephyr — Enumeration Diagnose
 *
 * Repliziert EXAKT den ST-Initialisierungspfad aus x-cube-n6-camera-capture.
 * Zweck: Wenn dieser Code enumeriert (STP>0), war unser Glue-Code das Problem.
 *
 * ST-Quellen (verbatim uebernommen/dokumentiert):
 *   HAL PCD Init:  Lib/uvcl/Src/uvcl.c              → UVCL_usb_init()
 *   USBX Init:     Lib/uvcl/Src/usbx/uvcl_usbx.c    → UVCL_usbx_init()
 *   ISR Muster:    Lib/uvcl/Src/usbx/uvcl_usbx.c    → UVCL_stm32_usbx_IRQHandler()
 *   ISR Muster 2:  Lib/Camera_Middleware/.../usbx.c  → usbx_device_cb (SOF-Trigger)
 *
 * Kritischer Unterschied zu unserem Code:
 *   ST-ISR ruft ux_system_tasks_run() ZWEIMAL nach HAL_PCD_IRQHandler() auf.
 *   Unser Code ruft es aus einem Thread mit k_sleep(K_MSEC(1)) auf.
 *   → XI640_ST_ISR_MODE=1 aktiviert die ST-kompatible ISR in xi640_dcd.c.
 *
 * Einzige Unterschiede zum ST-Original:
 *   - IRQ_CONNECT(177, ...) statt HAL_NVIC_EnableIRQ(USB1_OTG_HS_IRQn)
 *   - Zephyr LOG_INF statt printf
 *   - Unser Deskriptor-Code (xi640_uvc_descriptors.c) statt UVCL_get_*()
 *     (funktional aequivalent, testet Enumeration unabhaengig vom Beschreiber)
 *
 * Aktivierung: Setze in app/CMakeLists.txt:
 *   target_compile_definitions(app PRIVATE XI640_ST_ISR_MODE=1 XI640_ST_TEST_MODE=1)
 *   target_sources(app PRIVATE src/xi640_st_test.c)
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#include "xi640_st_test.h"
#include "xi640_dcd.h"
#include "xi640_uvc_stream.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(xi640_st_test, LOG_LEVEL_INF);

/* ── Diagnose-Zugriff auf ISR-Zaehler (aus xi640_dcd.c) ─────────────────── */
extern volatile uint32_t g_usb_irq_count;
extern volatile uint32_t g_usb_reset_count;
extern volatile uint32_t g_usb_setup_count;
extern volatile uint32_t g_usb_enum_done_count;
extern volatile uint32_t g_usb_sof_count;
extern volatile uint32_t g_reset_cb_count;

/* ── Streaming-Thread: nur fuer tatsaechlichen Video-Stream nach Enumeration */
/* Wird NICHT beim ST-Klon-Test gestartet — Enumeration braucht ihn nicht.   */
K_THREAD_STACK_DEFINE(xi640_st_stream_stack, 4096);
static struct k_thread xi640_st_stream_thread;

/**
 * @brief ST-Klon Init — ersetzt die 5-Schritt-Sequenz in main.c
 *
 * Abbildung auf ST-Code:
 *   Schritt 1 = UVCL_usb_init()              (HAL PCD + FIFO, identisch)
 *   Schritt 2 = UVCL_usbx_init() / usbx_init() (USBX System + Stack + Video)
 *   Schritt 3 = ux_dcd_stm32_initialize()    (DCD-Registrierung)
 *   Schritt 4 = KEIN tasks-Thread            (tasks_run aus ISR via XI640_ST_ISR_MODE=1)
 *   Schritt 5 = HAL_PCD_Start()              (IRQ enable + Enumeration)
 */
int xi640_st_init(void)
{
    xi640_dcd_status_t  dcd_ret;
    xi640_uvc_status_t  uvc_ret;

    LOG_INF("╔══════════════════════════════════════════════════════╗");
    LOG_INF("║         Xi640 ST-Klon Test (XI640_ST_TEST_MODE)      ║");
    LOG_INF("╚══════════════════════════════════════════════════════╝");
    LOG_INF("ISR-Modus: ux_system_tasks_run() x2 aus ISR (XI640_ST_ISR_MODE=%d)",
            (int)0 /* XI640_ST_ISR_MODE nicht gesetzt */);
    LOG_INF("Kein separater Task-Thread — wie ST-Referenz.");

#if !defined(XI640_ST_ISR_MODE) || (XI640_ST_ISR_MODE != 1)
    LOG_WRN("WARNUNG: XI640_ST_ISR_MODE ist nicht gesetzt!");
    LOG_WRN("ISR ruft tasks_run() NICHT auf — Enumeration wird fehlschlagen.");
    LOG_WRN("Setze XI640_ST_ISR_MODE=1 in CMakeLists.txt.");
#endif

    /* ─── Schritt 1: HAL PCD + FIFO init ──────────────────────────────── */
    /* Entspricht UVCL_usb_init() in Lib/uvcl/Src/uvcl.c:
     *   HAL_PCD_Init(pcd, USB1_OTG_HS, speed=HIGH, dma=ENABLE, phy=HS_EMBEDDED)
     *   HAL_PCDEx_SetRxFiFo(0x80)     // 128 Words RX
     *   HAL_PCDEx_SetTxFiFo(0, 0x10)  // 16 Words EP0 TX
     *   HAL_PCDEx_SetTxFiFo(1, 0x300) // 768 Words EP1 TX
     *   HAL_NVIC_EnableIRQ(...)        → bei uns: IRQ_CONNECT(177,...) in xi640_dcd_init
     * Wir rufen xi640_dcd_init() auf — macht EXAKT dasselbe. */
    dcd_ret = xi640_dcd_init();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("ST-Klon Schritt 1 (DCD HW Init) fehlgeschlagen: %d", dcd_ret);
        return -1;
    }
    LOG_INF("ST-Klon Schritt 1: HAL PCD + FIFO init OK (entspricht UVCL_usb_init)");

    /* ─── Schritt 2: USBX System + Device Stack + Video Class ─────────── */
    /* Entspricht UVCL_usbx_init() in Lib/uvcl/Src/usbx/uvcl_usbx.c:
     *   ux_system_initialize(pool, size, UX_NULL, 0)
     *   UVCL_get_device_desc / UVCL_get_configuration_desc
     *   ux_device_stack_initialize(hs_desc, fs_desc, strings, langid, cb)
     *   ux_device_stack_class_register(video_class, ...)
     * Wir rufen xi640_uvc_init() auf — macht dasselbe mit unseren Deskriptoren.
     * Deskriptoren sind funktional aequivalent (640x480 YUYV YUY2, 30fps, HS). */
    uvc_ret = xi640_uvc_init();
    if (uvc_ret != XI640_UVC_OK) {
        LOG_ERR("ST-Klon Schritt 2 (USBX Init) fehlgeschlagen: %d", uvc_ret);
        return -2;
    }
    LOG_INF("ST-Klon Schritt 2: USBX + Video Class init OK (entspricht UVCL_usbx_init)");

    /* ─── Schritt 3: DCD bei USBX registrieren ────────────────────────── */
    /* Entspricht: ux_dcd_stm32_initialize((ULONG)pcd_instance, (ULONG)pcd_handle)
     * am Ende von UVCL_usbx_init(). */
    dcd_ret = xi640_dcd_register_usbx();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("ST-Klon Schritt 3 (DCD Register) fehlgeschlagen: %d", dcd_ret);
        return -3;
    }
    LOG_INF("ST-Klon Schritt 3: DCD bei USBX registriert");

    /* ─── Schritt 4: KEIN Tasks-Thread ────────────────────────────────── */
    /* ST-Referenz startet KEINEN separaten Thread fuer ux_system_tasks_run().
     *
     * Stattdessen ruft UVCL_stm32_usbx_IRQHandler() tasks_run() ZWEIMAL
     * direkt nach HAL_PCD_IRQHandler() aus dem ISR-Kontext:
     *
     *   void UVCL_stm32_usbx_IRQHandler(void) {
     *       HAL_PCD_IRQHandler(&uvcl_pcd_handle);
     *       #ifdef UX_STANDALONE
     *       ux_system_tasks_run();   // ← Schritt 4a
     *       ux_system_tasks_run();   // ← Schritt 4b
     *       #endif
     *   }
     *
     * Dies ist der KRITISCHE Unterschied zu unserem Code!
     * In unserem Code laufen tasks_run() in einem Thread mit k_sleep(1ms).
     * Der Host erwartet Descriptor-Antworten innerhalb von ~500 µs.
     * Mit k_sleep(1ms) kann die Antwort bis zu 1ms verzoegert sein.
     *
     * Aktivierung: XI640_ST_ISR_MODE=1 in CMakeLists.txt modifiziert
     * xi640_usb_irq_handler() in xi640_dcd.c so dass tasks_run() aus ISR laeuft.
     */
    LOG_INF("ST-Klon Schritt 4: Kein tasks-Thread gestartet.");
    LOG_INF("  tasks_run() laeuft via XI640_ST_ISR_MODE=%d in xi640_usb_irq_handler().",
            (int)0 /* XI640_ST_ISR_MODE nicht gesetzt */);

    /* ─── Schritt 5: IRQ enable + HAL_PCD_Start → Enumeration beginnt ── */
    /* Entspricht HAL_PCD_Start() am Ende von UVCL_Init() in uvcl.c.
     * Bei uns: xi640_dcd_start() macht irq_enable(177) + HAL_PCD_Start(). */
    dcd_ret = xi640_dcd_start();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("ST-Klon Schritt 5 (DCD Start) fehlgeschlagen: %d", dcd_ret);
        return -5;
    }
    LOG_INF("ST-Klon Schritt 5: USB gestartet (IRQ aktiv, Enumeration beginnt)");

    LOG_INF("═══════════════════════════════════════════════════════");
    LOG_INF("ST-Klon Init abgeschlossen. Erwartung:");
    LOG_INF("  STP > 0 nach erstem USBRST → Enumeration laeuft");
    LOG_INF("  Windows Geraetemanager: 'STM32 uvc' unter USB 3.x");
    LOG_INF("═══════════════════════════════════════════════════════");

    return 0;
}
