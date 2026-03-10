/*
 * Xi640 Project 22 — main.c
 * STM32N6 Hybrid USBX/Zephyr UVC ISO
 *
 * Phase 4: DCD initialisiert, USB Enumeration aktiv
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "xi640_dcd.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    xi640_dcd_status_t ret;
    xi640_dcd_diag_t diag;

    LOG_INF("Xi640 Project 22 — USBX/Zephyr Hybrid");

    /* Phase 4: DCD Hardware-Init (Clock, PCD, FIFO, IRQ) */
    ret = xi640_dcd_init();
    if (ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_init fehlgeschlagen: %d", ret);
        goto error;
    }

    /*
     * TODO Phase 5: USBX Stack Init hier einfuegen:
     *   ux_system_initialize(...)
     *   ux_device_stack_initialize(...)
     *   xi640_dcd_register_usbx()
     */

    /* USB-Verbindung aktivieren — Enumeration beginnt */
    ret = xi640_dcd_start();
    if (ret != XI640_DCD_OK) {
        LOG_ERR("xi640_dcd_start fehlgeschlagen: %d", ret);
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
        LOG_ERR("DCD Init fehlgeschlagen — System gestoppt");
    }
    return -1;
}
