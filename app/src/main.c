/*
 * Xi640 Project 22 — main.c
 * STM32N6 Hybrid USBX/Zephyr UVC ISO
 *
 * Phase 1: Zephyr USB deaktiviert, USBX noch nicht aktiv
 * Ziel: System bootet stabil, kein Zephyr USB im Log
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("Xi640 Project 22 — USBX/Zephyr Hybrid");
    LOG_INF("Phase 1: Zephyr USB deaktiviert");
    LOG_INF("USB Controller freigegeben fuer USBX");

    /* TODO Phase 2: usbx_init(); */
    /* TODO Phase 3: usbx_port_init(); */
    /* TODO Phase 4: usbx_dcd_stm32_init(); */
    /* TODO Phase 5: uvc_iso_dummy_stream_start(); */

    while (1) {
        k_sleep(K_MSEC(1000));
        LOG_INF("Heartbeat — kein Zephyr USB Stack aktiv");
    }

    return 0;
}
