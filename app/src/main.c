/*
 * Xi640 Project 22 — main.c
 * STM32N6 Hybrid USBX/Zephyr UVC ISO
 *
 * Phase 5: UVC Colorbar Dummy-Stream
 *
 * Initialisierungsreihenfolge:
 *   1. xi640_dcd_init()          — HW Init (Clock, PCD, FIFO, IRQ verbinden)
 *   2. xi640_uvc_init()          — USBX System + Device Stack + Video Class
 *   3. xi640_dcd_register_usbx() — DCD bei USBX registrieren
 *   4. xi640_uvc_stream_start()  — Streaming-Thread starten (pumpt tasks_run)
 *   5. xi640_dcd_start()         — IRQ enablen + HAL_PCD_Start() -> Enumeration
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "xi640_dcd.h"
#include "xi640_uvc_stream.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    xi640_dcd_status_t  dcd_ret;
    xi640_uvc_status_t  uvc_ret;
    xi640_dcd_diag_t    diag;

    LOG_INF("Xi640 Project 22 — USBX/Zephyr UVC ISO");
    LOG_INF("Phase 5: UVC Colorbar Dummy-Stream");

    /* ─── 1. DCD Hardware-Init ─────────────────────────────────────── */
    dcd_ret = xi640_dcd_init();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_init fehlgeschlagen: %d", dcd_ret);
        goto error;
    }

    /* ─── 2. USBX + Video Class Init ──────────────────────────────── */
    uvc_ret = xi640_uvc_init();
    if (uvc_ret != XI640_UVC_OK) {
        LOG_ERR("xi640_uvc_init fehlgeschlagen: %d", uvc_ret);
        goto error;
    }

    /* ─── 3. DCD bei USBX registrieren ────────────────────────────── */
    dcd_ret = xi640_dcd_register_usbx();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_register_usbx fehlgeschlagen: %d", dcd_ret);
        goto error;
    }

    /* ─── 4. Streaming-Thread starten ─────────────────────────────── */
    /* Muss VOR xi640_dcd_start() laufen — pumpt ux_system_tasks_run() */
    /* fuer die USB-Enumeration (SETUP-Pakete verarbeiten).            */
    xi640_uvc_stream_start();

    /* ─── 5. USB-Verbindung aktivieren (Enumeration beginnt) ───────── */
    dcd_ret = xi640_dcd_start();
    if (dcd_ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_start fehlgeschlagen: %d", dcd_ret);
        goto error;
    }

    /* Diagnostik nach Init ausgeben */
    if (xi640_dcd_get_diagnostics(&diag) == XI640_DCD_OK) {
        LOG_INF("OTG Core ID: 0x%08x", diag.otg_core_id);
        LOG_INF("DMA: %s, Burst=0x%x",
                diag.dma_enabled ? "aktiv" : "aus", diag.dma_burst_len);
        LOG_INF("FIFO: RX=%u, EP0=%u, EP1=%u Words",
                diag.fifo_rx_words, diag.fifo_ep0_tx_words,
                diag.fifo_ep1_tx_words);
    }

    /* ─── Haupt-Loop: Diagnostik alle 5 Sekunden ──────────────────── */
    while (1) {
        k_sleep(K_MSEC(5000));

        if (xi640_dcd_get_diagnostics(&diag) == XI640_DCD_OK) {
            LOG_INF("USB: Speed=%u Frame=%u ISOinc=%u",
                    diag.enum_speed, diag.frame_number,
                    diag.iso_incomplete_cnt);
        }
    }

    return 0;

error:
    while (1) {
        k_sleep(K_MSEC(1000));
        LOG_ERR("Initialisierung fehlgeschlagen — System gestoppt");
    }
    return -1;
}
