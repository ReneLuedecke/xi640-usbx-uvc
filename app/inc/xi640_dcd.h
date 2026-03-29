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
 * FIFO Konfiguration — ST Referenzwerte (x-cube-n6-camera-capture, uvcl.c Zeile 294-296)
 *
 * Gesamt: 4096 Words verfuegbar
 * ST-Belegung: 0x80 + 0x10 + 0x300 = 912 Words (22%)
 *
 * Kleinere FIFOs = weniger Buffer-Nutzung, aber mehr Interrupts.
 * Fuer Enumeration ausreichend; fuer ISO Streaming ggf. anpassen.
 */
#define XI640_DCD_FIFO_TOTAL_WORDS     4096U
#define XI640_DCD_RX_FIFO_WORDS        0x80U   /**< RX FIFO:  128 Words (ST-Wert) */
#define XI640_DCD_EP0_TX_FIFO_WORDS    0x10U   /**< EP0 TX:    16 Words (ST-Wert) */
#define XI640_DCD_EP1_TX_FIFO_WORDS   0x300U   /**< EP1 TX:   768 Words (ST-Wert) */

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

/**
 * @brief   Liest USB OTG Interrupt-Masken-Register aus.
 *
 * Kapselt den HAL-Typ USB_OTG_GlobalTypeDef / USB_OTG_DeviceTypeDef,
 * damit main.c keinen direkten STM32-HAL-Include benoetigt.
 *
 * Zu pruefende Bits:
 *   GINTMSK  Bit  4 (RXFLVLM = 0x00000010): SETUP-IRQ Enable
 *   DOEPMSK  Bit  3 (STUPM   = 0x00000008): SETUP-Phase-Done Enable
 *   DAINTMSK Bit 16 (OEPINT0 = 0x00010000): EP0 OUT Interrupt Enable
 *
 * @param   gintmsk   [out] Wert von USB1_OTG_HS->GINTMSK
 * @param   doepmsk   [out] Wert von DOEPMSK
 * @param   daintmsk  [out] Wert von DAINTMSK
 * @note    NICHT im Hot Path aufrufen!
 */
void xi640_dcd_get_irq_masks(uint32_t *gintmsk, uint32_t *doepmsk, uint32_t *daintmsk);

/**
 * @brief   Liest rohe Bus-Statusregister (ungemaskiert).
 *
 * GINTSTS: alle aktuell gesetzten Interrupt-Flags (unabhaengig von GINTMSK).
 * GOTGCTL: OTG Control/Status — BSVLD (Bit 19 = 0x80000) zeigt ob VBUS
 *          vorhanden ist (d.h. Kabel an echtem USB-Host steckt).
 *
 * @param   gintsts  [out] Wert von USB1_OTG_HS->GINTSTS (raw)
 * @param   gotgctl  [out] Wert von USB1_OTG_HS->GOTGCTL
 * @note    NICHT im Hot Path aufrufen!
 */
void xi640_dcd_get_bus_regs(uint32_t *gintsts, uint32_t *gotgctl);

/**
 * @brief   Liest DCTL und PCGCCTL fuer Soft-Connect Diagnose.
 *
 * DCTL Bit 1 = SDIS (Soft Disconnect):
 *   0 = Device verbunden (D+ Pull-up aktiv) — Soll nach HAL_PCD_Start!
 *   1 = Soft Disconnect aktiv — Host sieht Device NICHT
 *
 * PCGCCTL Bits [1:0] = STOPCLK | GATECLK:
 *   Beide muessen 0 sein — sonst ist PHY-Clock gesperrt → kein USB-Betrieb.
 *
 * @param   dctl     [out] DCTL Register (pruefe Bit 1 = SDIS = 0)
 * @param   pcgcctl  [out] PCGCCTL Register (pruefe Bits [1:0] = 0)
 * @note    NICHT im Hot Path aufrufen!
 */
void xi640_dcd_get_dctl_pcgcctl(uint32_t *dctl, uint32_t *pcgcctl);

/**
 * @brief   Liest AHB/Device-Konfigurationsregister.
 *
 * GAHBCFG Bit 0 (GINTMSK = Global Interrupt Enable) MUSS nach HAL_PCD_Start = 1 sein.
 * Falls 0: USB-Core generiert keine Interrupts mehr (ENUMDNE stumm).
 * DCFG Bits [1:0]: 00=HS konfiguriert, 01=FS-in-HS-PHY.
 * GCCFG VBVALOVAL: 1 = VBUS Override aktiv (Sollwert bei vbus_sensing_enable=DISABLE).
 *
 * @param   gahbcfg  [out] GAHBCFG (pruefe Bit 0 = GINT)
 * @param   dcfg     [out] DCFG (pruefe Bits [1:0] = DEVSPD)
 * @param   gccfg    [out] GCCFG (pruefe VBVALOVAL und VBVALEXTOEN)
 */
void xi640_dcd_get_ahb_regs(uint32_t *gahbcfg, uint32_t *dcfg, uint32_t *gccfg);

/**
 * @brief   Liest EP0 OUT Interrupt-Register und DAINT.
 *
 * Kapselt DAINT und die DOEP*-Register fuer EP0 OUT Diagnostik.
 * Relevante Bits in DOEPINT0:
 *   Bit 3 (STUP  = 0x08): SETUP-Paket empfangen, aber noch nicht verarbeitet.
 *                          Falls gesetzt bei STP=0: EP0 OUT IRQ kommt nicht durch.
 *   Bit 0 (XFRC  = 0x01): Transfer Complete
 *
 * @param   daint      [out] DAINT — welche Endpoints aktive Interrupts haben
 * @param   doepctl0   [out] DOEPCTL0 — EP0 OUT Steuerregister
 * @param   doepint0   [out] DOEPINT0 — EP0 OUT Interrupt-Flags (raw, vor DOEPMSK)
 * @param   doeptsiz0  [out] DOEPTSIZ0 — EP0 OUT Transfer-Groesse/Paket-Zaehler
 * @note    NICHT im Hot Path aufrufen!
 */
void xi640_dcd_get_ep0_out_regs(uint32_t *daint, uint32_t *doepctl0,
                                  uint32_t *doepint0, uint32_t *doeptsiz0,
                                  uint32_t *doepdma0);

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
    uint32_t    usb_irq_count;      /**< Gesamt-IRQ-Aufrufe seit Boot */
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
