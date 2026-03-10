/**
 * @file    xi640_dcd.c
 * @brief   Xi640 USB OTG HS Device Controller Driver — Implementierung
 *
 * Konfiguriert den STM32N6 USB OTG HS Controller fuer USBX UVC ISO Streaming.
 * Dieser Modul ist der Glue-Layer zwischen USBX (ux_dcd_stm32) und der
 * STM32N6 HAL PCD.
 *
 * NICHT geaendert: ux_dcd_stm32_*.c (USBX DCD Treiber).
 *
 * Initialisierungsreihenfolge:
 *   1. xi640_dcd_init()              — HW-Init (Clock, PCD, FIFO, IRQ)
 *   2. ux_system_initialize()        — USBX System
 *   3. ux_device_stack_initialize()  — USBX Device Stack
 *   4. xi640_dcd_register_usbx()     — DCD bei USBX registrieren
 *   5. xi640_dcd_start()             — USB-Verbindung aktivieren
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#include "xi640_dcd.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

/* STM32N6 HAL PCD */
#include <stm32n6xx_hal.h>
#include <stm32n6xx_hal_pcd.h>
#include <stm32n6xx_hal_pcd_ex.h>

/* USBX API + STM32 DCD — ux_api.h muss vor ux_dcd_stm32.h stehen */
#include "ux_api.h"
#include "ux_dcd_stm32.h"

LOG_MODULE_REGISTER(xi640_dcd, CONFIG_LOG_DEFAULT_LEVEL);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Statische Compile-Time Validierung                                     */
/* ──────────────────────────────────────────────────────────────────────── */

_Static_assert(
    (XI640_DCD_RX_FIFO_WORDS + XI640_DCD_EP0_TX_FIFO_WORDS + XI640_DCD_EP1_TX_FIFO_WORDS)
    <= XI640_DCD_FIFO_TOTAL_WORDS,
    "FIFO Konfiguration ueberschreitet Gesamt-Budget von 4096 Words"
);

_Static_assert(
    (XI640_DCD_ISO_EP_ADDR & 0x80U) != 0U,
    "XI640_DCD_ISO_EP_ADDR muss Bit 7 gesetzt haben (IN Richtung)"
);

_Static_assert(
    (((XI640_DCD_ISO_WMAXPACKETSIZE >> 11U) & 0x3U) + 1U) == 3U,
    "XI640_DCD_ISO_WMAXPACKETSIZE muss 3 Transaktionen kodieren"
);

_Static_assert(
    (XI640_DCD_ISO_TRANSFER_SIZE % XI640_DCD_CACHE_LINE_SIZE) == 0U,
    "XI640_DCD_ISO_TRANSFER_SIZE muss 32-Byte aligned sein"
);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Modul-interne Zustandsvariablen                                        */
/* ──────────────────────────────────────────────────────────────────────── */

static PCD_HandleTypeDef hpcd;

/** ISO IN Incomplete Events — atomar fuer ISR-Sicherheit */
static atomic_t iso_incomplete_count;

/* ──────────────────────────────────────────────────────────────────────── */
/*  HAL_PCD_MspInit — weak Override fuer Clock/PHY Setup                  */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief HAL MSP Init fuer USB OTG HS — Clock und PHY aktivieren.
 *
 * Ueberschreibt die __weak HAL-Implementierung.
 * NVIC wird NICHT hier konfiguriert — das macht Zephyr via IRQ_CONNECT.
 * GPIO-Konfiguration ist beim embedded HS PHY nicht noetig (dedizierte Pins).
 */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd_arg)
{
    (void)hpcd_arg;

    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();

    LOG_DBG("USB1 OTG HS + PHY Clocks aktiviert");
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd_arg)
{
    (void)hpcd_arg;

    __HAL_RCC_USB1_OTG_HS_CLK_DISABLE();
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_DISABLE();

    LOG_DBG("USB1 OTG HS + PHY Clocks deaktiviert");
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  ISO Incomplete Callback (Hot Path — kein LOG!)                        */
/* ──────────────────────────────────────────────────────────────────────── */

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd_arg, uint8_t epnum)
{
    (void)hpcd_arg;
    (void)epnum;

    atomic_inc(&iso_incomplete_count);
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Oeffentliche API                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

xi640_dcd_status_t xi640_dcd_init(void)
{
    HAL_StatusTypeDef hal_ret;

    LOG_INF("USB OTG HS DCD Initialisierung gestartet");

    /* ─── Schritt 1: PCD Handle konfigurieren ─────────────────────────── */
    hpcd.Instance = XI640_DCD_USB_INSTANCE;

    hpcd.Init.dev_endpoints = XI640_DCD_NUM_ENDPOINTS;
    hpcd.Init.Host_channels = 0U;
    hpcd.Init.dma_enable    = 1U;
    hpcd.Init.speed         = USB_OTG_SPEED_HIGH;
    hpcd.Init.phy_itface    = USB_OTG_HS_EMBEDDED_PHY;
    hpcd.Init.Sof_enable    = 1U;
    hpcd.Init.ep0_mps       = USB_OTG_MAX_EP0_SIZE;
    hpcd.Init.low_power_enable = 0U;
    hpcd.Init.lpm_enable    = 0U;
    hpcd.Init.battery_charging_enable = 0U;
    hpcd.Init.vbus_sensing_enable = 0U;
    hpcd.Init.use_dedicated_ep1 = 0U;
    hpcd.Init.use_external_vbus = 0U;

    /* ─── Schritt 2: HAL_PCD_Init ─────────────────────────────────────── */
    hal_ret = HAL_PCD_Init(&hpcd);
    if (hal_ret != HAL_OK) {
        LOG_ERR("HAL_PCD_Init fehlgeschlagen: %d", hal_ret);
        return XI640_DCD_ERR_PCD_INIT;
    }
    LOG_DBG("HAL_PCD_Init OK");

    /* ─── Schritt 3: FIFO konfigurieren ───────────────────────────────── */
    hal_ret = HAL_PCDEx_SetRxFiFo(&hpcd, XI640_DCD_RX_FIFO_WORDS);
    if (hal_ret != HAL_OK) {
        LOG_ERR("HAL_PCDEx_SetRxFiFo fehlgeschlagen: %d", hal_ret);
        return XI640_DCD_ERR_FIFO;
    }

    hal_ret = HAL_PCDEx_SetTxFiFo(&hpcd, 0U, XI640_DCD_EP0_TX_FIFO_WORDS);
    if (hal_ret != HAL_OK) {
        LOG_ERR("HAL_PCDEx_SetTxFiFo EP0 fehlgeschlagen: %d", hal_ret);
        return XI640_DCD_ERR_FIFO;
    }

    hal_ret = HAL_PCDEx_SetTxFiFo(&hpcd, XI640_DCD_ISO_EP_NUM,
                                   XI640_DCD_EP1_TX_FIFO_WORDS);
    if (hal_ret != HAL_OK) {
        LOG_ERR("HAL_PCDEx_SetTxFiFo EP1 fehlgeschlagen: %d", hal_ret);
        return XI640_DCD_ERR_FIFO;
    }
    LOG_DBG("FIFO: RX=%u, EP0=%u, EP1=%u Words",
            XI640_DCD_RX_FIFO_WORDS,
            XI640_DCD_EP0_TX_FIFO_WORDS,
            XI640_DCD_EP1_TX_FIFO_WORDS);

    /* ─── Schritt 4: Zephyr ISR verbinden ─────────────────────────────── */
    IRQ_CONNECT(XI640_DCD_IRQ_NUM, XI640_DCD_IRQ_PRIO,
                _ux_dcd_stm32_interrupt_handler, NULL, 0);
    irq_enable(XI640_DCD_IRQ_NUM);

    LOG_INF("USB OTG HS DCD bereit (IRQ %d, Prio %d)",
            XI640_DCD_IRQ_NUM, XI640_DCD_IRQ_PRIO);

    return XI640_DCD_OK;
}

xi640_dcd_status_t xi640_dcd_register_usbx(void)
{
    /*
     * _ux_dcd_stm32_initialize setzt voraus dass ux_system_initialize()
     * und ux_device_stack_initialize() bereits gelaufen sind.
     * Deshalb ist diese Funktion GETRENNT von xi640_dcd_init().
     */
    UINT ux_ret = _ux_dcd_stm32_initialize(0UL, (ULONG)(uintptr_t)&hpcd);

    if (ux_ret != UX_SUCCESS) {
        LOG_ERR("_ux_dcd_stm32_initialize fehlgeschlagen: 0x%x", ux_ret);
        return XI640_DCD_ERR_USBX_INIT;
    }
    LOG_INF("USBX DCD registriert");
    return XI640_DCD_OK;
}

xi640_dcd_status_t xi640_dcd_start(void)
{
    HAL_StatusTypeDef ret = HAL_PCD_Start(&hpcd);

    if (ret != HAL_OK) {
        LOG_ERR("HAL_PCD_Start fehlgeschlagen: %d", ret);
        return XI640_DCD_ERR_START;
    }

    LOG_INF("USB OTG HS gestartet — Enumeration aktiv");
    return XI640_DCD_OK;
}

xi640_dcd_status_t xi640_dcd_stop(void)
{
    HAL_StatusTypeDef ret = HAL_PCD_Stop(&hpcd);

    if (ret != HAL_OK) {
        LOG_ERR("HAL_PCD_Stop fehlgeschlagen: %d", ret);
        return XI640_DCD_ERR_STOP;
    }

    LOG_INF("USB OTG HS gestoppt");
    return XI640_DCD_OK;
}

void *xi640_dcd_get_pcd_handle(void)
{
    return &hpcd;
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Cache-Maintenance Wrapper                                              */
/* ──────────────────────────────────────────────────────────────────────── */

void xi640_dcd_cache_flush(const void *buf, uint32_t size)
{
    __ASSERT(buf != NULL, "Cache-Flush: Buffer NULL");
    __ASSERT(((uintptr_t)buf % XI640_DCD_CACHE_LINE_SIZE) == 0U,
             "Cache-Flush: Buffer nicht 32-Byte aligned (0x%p)", buf);

    size_t aligned_size = (size_t)XI640_DCD_CACHE_ALIGN_SIZE(size);
    sys_cache_data_flush_range((void *)buf, aligned_size);
}

void xi640_dcd_cache_invalidate(void *buf, uint32_t size)
{
    __ASSERT(buf != NULL, "Cache-Invalidate: Buffer NULL");
    __ASSERT(((uintptr_t)buf % XI640_DCD_CACHE_LINE_SIZE) == 0U,
             "Cache-Invalidate: Buffer nicht 32-Byte aligned (0x%p)", buf);

    size_t aligned_size = (size_t)XI640_DCD_CACHE_ALIGN_SIZE(size);
    sys_cache_data_invd_range(buf, aligned_size);
}

/* ──────────────────────────────────────────────────────────────────────── */
/*  Diagnostik (nur Cold Path!)                                            */
/* ──────────────────────────────────────────────────────────────────────── */

xi640_dcd_status_t xi640_dcd_get_diagnostics(xi640_dcd_diag_t *diag)
{
    USB_OTG_GlobalTypeDef *usb = XI640_DCD_USB_INSTANCE;
    USB_OTG_DeviceTypeDef *dev;

    if (diag == NULL) {
        return XI640_DCD_ERR_PARAM;
    }

    dev = (USB_OTG_DeviceTypeDef *)((uint32_t)usb + USB_OTG_DEVICE_BASE);

    /* IRQ-Sperre fuer konsistenten Snapshot */
    unsigned int key = irq_lock();

    diag->otg_core_id      = usb->GSNPSID;
    diag->enum_speed        = (dev->DSTS >> 1U) & 0x3U;
    diag->fifo_rx_words     = usb->GRXFSIZ & 0xFFFFU;
    diag->fifo_ep0_tx_words = (usb->DIEPTXF0_HNPTXFSIZ >> 16U) & 0xFFFFU;
    diag->fifo_ep1_tx_words = (usb->DIEPTXF[0] >> 16U) & 0xFFFFU;
    diag->dma_burst_len     = (usb->GAHBCFG >> 1U) & 0xFU;
    diag->dma_enabled       = (usb->GAHBCFG >> 5U) & 0x1U;
    diag->frame_number      = hpcd.FrameNumber;

    irq_unlock(key);

    diag->iso_incomplete_cnt = (uint32_t)atomic_get(&iso_incomplete_count);

    return XI640_DCD_OK;
}

xi640_dcd_status_t xi640_dcd_set_dma_burst(uint32_t hbstlen)
{
    USB_OTG_GlobalTypeDef *usb = XI640_DCD_USB_INSTANCE;

    if (hbstlen != XI640_DCD_DMA_BURST_INCR4  &&
        hbstlen != XI640_DCD_DMA_BURST_INCR8  &&
        hbstlen != XI640_DCD_DMA_BURST_INCR16) {
        LOG_ERR("Ungueltiger HBSTLEN-Wert 0x%x", hbstlen);
        return XI640_DCD_ERR_PARAM;
    }

    /* IRQ-Lock: RMW auf GAHBCFG muss atomar sein */
    unsigned int key = irq_lock();

    uint32_t gahbcfg = usb->GAHBCFG;
    gahbcfg &= ~USB_OTG_GAHBCFG_HBSTLEN_Msk;
    gahbcfg |= (hbstlen << USB_OTG_GAHBCFG_HBSTLEN_Pos);
    usb->GAHBCFG = gahbcfg;

    irq_unlock(key);

    LOG_INF("DMA Burst: HBSTLEN=0x%x (%s)", hbstlen,
            (hbstlen == XI640_DCD_DMA_BURST_INCR16) ? "INCR16" :
            (hbstlen == XI640_DCD_DMA_BURST_INCR8)  ? "INCR8"  : "INCR4");

    return XI640_DCD_OK;
}
