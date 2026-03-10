/**
 * @file    xi640_dcd.h
 * @brief   Xi640 USB OTG HS Device Controller Driver — Interface
 *
 * Konfiguriert den STM32N6 USB OTG HS Controller fuer USBX UVC ISO Streaming.
 * Dieser Modul ist die Bruecke zwischen USBX (ux_dcd_stm32) und der
 * STM32N6 HAL PCD. Er kuemmert sich um:
 * - Clock/PHY Initialisierung
 * - FIFO Konfiguration
 * - DMA Burst-Einstellung
 * - Cache-Maintenance Wrapper
 * - Zephyr ISR Anbindung
 *
 * Der USBX DCD Treiber selbst (ux_dcd_stm32_*.c) bleibt UNVERAENDERT.
 *
 * Initialisierungsreihenfolge:
 *   1. xi640_dcd_init()          — einmal beim Boot
 *   2. USBX Stack Init          — ux_system_initialize, ux_device_stack_initialize
 *   3. xi640_dcd_start()         — USB-Verbindung aktivieren
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#ifndef XI640_DCD_H
#define XI640_DCD_H

#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────── */
/*  Rueckgabewerte                                                        */
/* ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    XI640_DCD_OK              = 0,
    XI640_DCD_ERR_CLOCK       = -1,
    XI640_DCD_ERR_PCD_INIT    = -2,
    XI640_DCD_ERR_FIFO        = -3,
    XI640_DCD_ERR_USBX_INIT  = -4,
    XI640_DCD_ERR_START       = -5,
    XI640_DCD_ERR_STOP        = -6,
    XI640_DCD_ERR_PARAM       = -7,
} xi640_dcd_status_t;

/* ──────────────────────────────────────────────────────────────────────── */
/*  Konfigurationskonstanten                                               */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * FIFO Konfiguration
 *
 * Gesamt: 4096 Words (16384 Bytes)
 * Belegung: 1024 + 512 + 2048 = 3584 Words (87.5%)
 * Frei: 512 Words fuer zukuenftige EPs
 */
#define XI640_DCD_FIFO_TOTAL_WORDS     4096U
#define XI640_DCD_RX_FIFO_WORDS        1024U   /**< RX FIFO: 4096 Bytes */
#define XI640_DCD_EP0_TX_FIFO_WORDS     512U   /**< EP0 TX: 2048 Bytes (Control) */
#define XI640_DCD_EP1_TX_FIFO_WORDS    2048U   /**< EP1 TX: 8192 Bytes (ISO UVC) */

/**
 * DMA Konfiguration
 *
 * HBSTLEN Werte (GAHBCFG Bits [4:1]):
 *   0x3 = INCR4  (4 Words = 16 Bytes) — HAL Default
 *   0x5 = INCR8  (8 Words = 32 Bytes)
 *   0x7 = INCR16 (16 Words = 64 Bytes) — Zielwert fuer max Bandbreite
 *
 * Strategie: Mit INCR4 starten, nach erfolgreicher Enumeration hochgehen.
 */
#define XI640_DCD_DMA_BURST_INCR4      0x3U
#define XI640_DCD_DMA_BURST_INCR8      0x5U
#define XI640_DCD_DMA_BURST_INCR16     0x7U
#define XI640_DCD_DMA_BURST_DEFAULT    XI640_DCD_DMA_BURST_INCR4

/**
 * ISO Endpoint Konfiguration
 */
/** wMaxPacketSize: 1024 * 3 Transactions = 0x1400 (High-Bandwidth ISO) */
#define XI640_DCD_ISO_WMAXPACKETSIZE   0x1400U
/** ISO Transfer-Blockgroesse: 16 KB */
#define XI640_DCD_ISO_TRANSFER_SIZE    16384U
/** ISO Endpoint Nummer (IN) */
#define XI640_DCD_ISO_EP_NUM           1U
/** ISO Endpoint Adresse (IN = 0x80 | EP_NUM) */
#define XI640_DCD_ISO_EP_ADDR          (0x80U | XI640_DCD_ISO_EP_NUM)

/**
 * PHY Konfiguration
 */
/** USB1 OTG HS Instanz (Board: CN18 USB-C Connector) */
#define XI640_DCD_USB_INSTANCE         USB1_OTG_HS
/** Eingebetteter HS PHY (kein externes ULPI) */
#define XI640_DCD_PHY_TYPE             USB_OTG_HS_EMBEDDED_PHY
/** Anzahl bidirektionale Endpoints (lt. DTS) */
#define XI640_DCD_NUM_ENDPOINTS        9U
/** USB OTG HS IRQ Nummer */
#define XI640_DCD_IRQ_NUM              177
/** USB OTG HS IRQ Prioritaet */
#define XI640_DCD_IRQ_PRIO             2

/**
 * Cache-Maintenance
 *
 * Cortex-M55: 32-Byte Cache-Line.
 * ALLE DMA-Buffer muessen 32-Byte aligned und Vielfache von 32 Bytes gross sein.
 */
#define XI640_DCD_CACHE_LINE_SIZE      32U

#define XI640_DCD_CACHE_ALIGN_SIZE(s)  (((s) + XI640_DCD_CACHE_LINE_SIZE - 1U) \
                                        & ~(XI640_DCD_CACHE_LINE_SIZE - 1U))

#define XI640_DCD_CACHE_ALIGNED        __attribute__((aligned(XI640_DCD_CACHE_LINE_SIZE)))

/* ──────────────────────────────────────────────────────────────────────── */
/*  Oeffentliche API                                                       */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief   Initialisiert den USB OTG HS Device Controller.
 *
 * Schritte:
 *   1. Clocks enablen (USB OTG HS + USB PHY Controller)
 *   2. PCD_HandleTypeDef konfigurieren
 *   3. HAL_PCD_Init()
 *   4. FIFO konfigurieren
 *   5. _ux_dcd_stm32_initialize() bei USBX registrieren
 *   6. Zephyr IRQ_CONNECT + irq_enable
 *
 * NICHT aufgerufen: HAL_PCD_Start() — dafuer xi640_dcd_start() nutzen.
 *
 * @return  XI640_DCD_OK bei Erfolg, Fehlercode sonst
 * @note    Muss VOR ux_system_initialize() aufgerufen werden
 */
xi640_dcd_status_t xi640_dcd_init(void);

/**
 * @brief   Registriert den DCD bei USBX.
 *
 * Muss NACH ux_system_initialize() + ux_device_stack_initialize() aufgerufen
 * werden, da _ux_dcd_stm32_initialize() den USBX Device Stack voraussetzt.
 *
 * @return  XI640_DCD_OK bei Erfolg, XI640_DCD_ERR_USBX_INIT sonst
 */
xi640_dcd_status_t xi640_dcd_register_usbx(void);

/**
 * @brief   Aktiviert die USB-Verbindung (Pull-Up auf D+).
 *
 * Ruft HAL_PCD_Start() auf. Ab hier beginnt Enumeration.
 *
 * @return  XI640_DCD_OK bei Erfolg
 * @note    Muss NACH vollstaendiger USBX Stack Init aufgerufen werden
 */
xi640_dcd_status_t xi640_dcd_start(void);

/**
 * @brief   Deaktiviert die USB-Verbindung.
 * @return  XI640_DCD_OK bei Erfolg
 */
xi640_dcd_status_t xi640_dcd_stop(void);

/**
 * @brief   Gibt Zeiger auf PCD Handle zurueck (fuer Diagnostik).
 * @return  Zeiger auf internes PCD_HandleTypeDef, oder NULL
 * @note    Nur fuer Diagnostik, nicht im Hot Path!
 */
void *xi640_dcd_get_pcd_handle(void);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Cache-Maintenance Wrapper                                              */
/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief   Cache-Flush vor DMA TX (CPU -> Memory).
 *
 * Buffer-Adresse wird auf Cache-Line abgerundet, Groesse aufgerundet.
 *
 * @param   buf     Zeiger auf DMA TX Buffer (muss 32-Byte aligned sein)
 * @param   size    Groesse in Bytes
 * @note    VOR jedem HAL_PCD_EP_Transmit() aufrufen!
 */
void xi640_dcd_cache_flush(const void *buf, uint32_t size);

/**
 * @brief   Cache-Invalidate nach DMA RX (Memory -> CPU).
 *
 * @param   buf     Zeiger auf DMA RX Buffer (muss 32-Byte aligned sein)
 * @param   size    Groesse in Bytes
 * @note    NACH jedem DMA RX Complete aufrufen!
 */
void xi640_dcd_cache_invalidate(void *buf, uint32_t size);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Diagnostik (nur ausserhalb Hot Path!)                                  */
/* ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t    otg_core_id;        /**< GSNPSID Register */
    uint32_t    enum_speed;         /**< 0=HS, 1=FS, 3=LS nach Enumeration */
    uint32_t    fifo_rx_words;
    uint32_t    fifo_ep0_tx_words;
    uint32_t    fifo_ep1_tx_words;
    uint32_t    dma_burst_len;      /**< HBSTLEN Wert aus GAHBCFG */
    uint32_t    dma_enabled;        /**< 1 wenn DMA aktiv */
    uint32_t    frame_number;       /**< Aktueller SOF Frame Number */
    uint32_t    iso_incomplete_cnt; /**< ISO IN Incomplete Events */
} xi640_dcd_diag_t;

/**
 * @brief   Liest DCD Diagnostik-Informationen.
 * @param   diag    Zeiger auf Zielstruktur
 * @return  XI640_DCD_OK bei Erfolg
 * @note    NICHT im Hot Path aufrufen!
 */
xi640_dcd_status_t xi640_dcd_get_diagnostics(xi640_dcd_diag_t *diag);

/**
 * @brief   Setzt DMA Burst-Laenge (fuer Tuning).
 *
 * Erlaubt Umschalten zwischen INCR4/INCR8/INCR16 zur Laufzeit.
 *
 * @param   hbstlen  XI640_DCD_DMA_BURST_INCR4, _INCR8, oder _INCR16
 * @return  XI640_DCD_OK bei Erfolg
 * @warning USB muss gestoppt sein oder kein Transfer aktiv!
 */
xi640_dcd_status_t xi640_dcd_set_dma_burst(uint32_t hbstlen);

#ifdef __cplusplus
}
#endif

#endif /* XI640_DCD_H */
