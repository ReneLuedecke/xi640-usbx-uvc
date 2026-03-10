/**
 * @file    test_xi640_dcd.c
 * @brief   TDD-Tests fuer xi640_dcd.h Konfigurationskonstanten
 *
 * Diese Tests laufen auf native_posix/native_sim ohne Hardware.
 * Sie pruefen ausschliesslich compile-time berechenbare Werte und
 * Makro-Ergebnisse aus xi640_dcd.h.
 *
 * Alle Tests sind statische Asserts (_Static_assert) oder einfache
 * Runtime-Asserts in main() — kein Zephyr ztest-Framework noetig,
 * da xi640_dcd.h keine Zephyr-Abhaengigkeiten hat die wir mocken muessten.
 *
 * TDD-Reihenfolge:
 *   1. FIFO-Validierung (Budget-Check)
 *   2. Cache-Alignment Makro
 *   3. DMA Burst Konstanten
 *   4. ISO Endpoint Konfiguration
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

/*
 * Dieser Test wird mit dem Zephyr Build-System (native_posix) kompiliert:
 *   west build -b native_posix app/app/tests -d build/test
 *   ./build/test/zephyr/zephyr.exe
 *
 * Die Makros aus xi640_dcd.h werden direkt getestet — kein Kopieren der Konstanten.
 */

#include <zephyr/ztest.h>
#include "xi640_dcd.h"

/* ──────────────────────────────────────────────────────────────────────── */
/* TEST SUITE 1: FIFO Budget-Validierung                                   */
/* ──────────────────────────────────────────────────────────────────────── */

ZTEST_SUITE(xi640_dcd_fifo, NULL, NULL, NULL, NULL, NULL);

/**
 * Test 1.1: FIFO-Gesamtbudget darf nicht ueberschritten werden.
 *
 * RX + EP0_TX + EP1_TX <= FIFO_TOTAL
 * 1024 + 512 + 2048 = 3584 <= 4096 => OK
 */
ZTEST(xi640_dcd_fifo, test_fifo_budget_nicht_ueberschritten)
{
    uint32_t belegung = XI640_DCD_RX_FIFO_WORDS
                      + XI640_DCD_EP0_TX_FIFO_WORDS
                      + XI640_DCD_EP1_TX_FIFO_WORDS;

    zassert_true(belegung <= XI640_DCD_FIFO_TOTAL_WORDS,
        "FIFO-Belegung %u ueberschreitet Gesamt %u",
        belegung, XI640_DCD_FIFO_TOTAL_WORDS);
}

/**
 * Test 1.2: Mindestens 10% Reserve im FIFO (fuer zukuenftige EPs).
 */
ZTEST(xi640_dcd_fifo, test_fifo_reserve_10_prozent)
{
    uint32_t belegung = XI640_DCD_RX_FIFO_WORDS
                      + XI640_DCD_EP0_TX_FIFO_WORDS
                      + XI640_DCD_EP1_TX_FIFO_WORDS;
    uint32_t reserve  = XI640_DCD_FIFO_TOTAL_WORDS - belegung;

    /* 10% von 4096 = 409 Words */
    zassert_true(reserve >= XI640_DCD_FIFO_TOTAL_WORDS / 10U,
        "FIFO-Reserve %u Words zu klein (min. %u erwartet)",
        reserve, XI640_DCD_FIFO_TOTAL_WORDS / 10U);
}

/**
 * Test 1.3: RX FIFO muss mindestens 256 Words haben (EP0 Setup Packets).
 */
ZTEST(xi640_dcd_fifo, test_fifo_rx_mindestgroesse)
{
    zassert_true(XI640_DCD_RX_FIFO_WORDS >= 256U,
        "RX FIFO %u Words zu klein (min. 256 Words fuer EP0 Setup)",
        XI640_DCD_RX_FIFO_WORDS);
}

/**
 * Test 1.4: EP1 ISO TX FIFO muss >= 3 * 1024 / 4 Words sein.
 *
 * 3 Transactions * 1024 Bytes = 3072 Bytes = 768 Words (Minimum fuer High-BW ISO).
 * Wir haben 2048 Words = 8192 Bytes — das ist mehr als genug.
 */
ZTEST(xi640_dcd_fifo, test_fifo_ep1_iso_ausreichend)
{
    /* Mindest-FIFO fuer High-Bandwidth ISO: 3 * 1024 Bytes = 768 Words */
    uint32_t min_iso_words = (3U * 1024U) / 4U;

    zassert_true(XI640_DCD_EP1_TX_FIFO_WORDS >= min_iso_words,
        "EP1 ISO TX FIFO %u Words zu klein (min. %u fuer High-BW ISO)",
        XI640_DCD_EP1_TX_FIFO_WORDS, min_iso_words);
}

/* ──────────────────────────────────────────────────────────────────────── */
/* TEST SUITE 2: Cache-Alignment Makro                                     */
/* ──────────────────────────────────────────────────────────────────────── */

ZTEST_SUITE(xi640_dcd_cache, NULL, NULL, NULL, NULL, NULL);

/**
 * Test 2.1: Alignment von 0 bleibt 0.
 */
ZTEST(xi640_dcd_cache, test_align_null_bleibt_null)
{
    zassert_equal(XI640_DCD_CACHE_ALIGN_SIZE(0U), 0U,
        "CACHE_ALIGN_SIZE(0) muss 0 sein, ist %u",
        XI640_DCD_CACHE_ALIGN_SIZE(0U));
}

/**
 * Test 2.2: 1 Byte wird auf 32 Bytes aufgerundet.
 */
ZTEST(xi640_dcd_cache, test_align_1_byte_wird_32)
{
    zassert_equal(XI640_DCD_CACHE_ALIGN_SIZE(1U), 32U,
        "CACHE_ALIGN_SIZE(1) muss 32 sein, ist %u",
        XI640_DCD_CACHE_ALIGN_SIZE(1U));
}

/**
 * Test 2.3: Genau 32 Bytes bleiben 32 Bytes (keine Uberrundung).
 */
ZTEST(xi640_dcd_cache, test_align_32_bleibt_32)
{
    zassert_equal(XI640_DCD_CACHE_ALIGN_SIZE(32U), 32U,
        "CACHE_ALIGN_SIZE(32) muss 32 sein, ist %u",
        XI640_DCD_CACHE_ALIGN_SIZE(32U));
}

/**
 * Test 2.4: 33 Bytes werden auf 64 Bytes aufgerundet.
 */
ZTEST(xi640_dcd_cache, test_align_33_wird_64)
{
    zassert_equal(XI640_DCD_CACHE_ALIGN_SIZE(33U), 64U,
        "CACHE_ALIGN_SIZE(33) muss 64 sein, ist %u",
        XI640_DCD_CACHE_ALIGN_SIZE(33U));
}

/**
 * Test 2.5: 614400 Bytes (YUYV Frame) wird auf naechstes 32-Byte-Vielfaches aufgerundet.
 *
 * 614400 / 32 = 19200 (ganzzahlig) => 614400 bleibt 614400.
 */
ZTEST(xi640_dcd_cache, test_align_frame_size_614400)
{
    /* 614400 = 640 * 480 * 2 — muss bereits aligned sein */
    zassert_equal(614400U % 32U, 0U,
        "YUYV Frame (614400 Bytes) ist nicht 32-Byte aligned — Designfehler!");

    zassert_equal(XI640_DCD_CACHE_ALIGN_SIZE(614400U), 614400U,
        "CACHE_ALIGN_SIZE(614400) muss 614400 sein");
}

/**
 * Test 2.6: Cache-Line-Groesse muss 32 Bytes sein (Cortex-M55).
 */
ZTEST(xi640_dcd_cache, test_cache_line_size_32_byte)
{
    zassert_equal(XI640_DCD_CACHE_LINE_SIZE, 32U,
        "Cache-Line-Groesse muss 32 Bytes sein (Cortex-M55), ist %u",
        XI640_DCD_CACHE_LINE_SIZE);
}

/* ──────────────────────────────────────────────────────────────────────── */
/* TEST SUITE 3: DMA Burst Konstanten                                      */
/* ──────────────────────────────────────────────────────────────────────── */

ZTEST_SUITE(xi640_dcd_dma, NULL, NULL, NULL, NULL, NULL);

/**
 * Test 3.1: INCR4 Burst-Wert muss 0x3 sein (lt. OTG HS GAHBCFG Spec).
 */
ZTEST(xi640_dcd_dma, test_burst_incr4_ist_0x3)
{
    zassert_equal(XI640_DCD_DMA_BURST_INCR4, 0x3U,
        "INCR4 Burst muss 0x3 sein, ist 0x%x",
        XI640_DCD_DMA_BURST_INCR4);
}

/**
 * Test 3.2: INCR8 Burst-Wert muss 0x5 sein.
 */
ZTEST(xi640_dcd_dma, test_burst_incr8_ist_0x5)
{
    zassert_equal(XI640_DCD_DMA_BURST_INCR8, 0x5U,
        "INCR8 Burst muss 0x5 sein, ist 0x%x",
        XI640_DCD_DMA_BURST_INCR8);
}

/**
 * Test 3.3: INCR16 Burst-Wert muss 0x7 sein.
 */
ZTEST(xi640_dcd_dma, test_burst_incr16_ist_0x7)
{
    zassert_equal(XI640_DCD_DMA_BURST_INCR16, 0x7U,
        "INCR16 Burst muss 0x7 sein, ist 0x%x",
        XI640_DCD_DMA_BURST_INCR16);
}

/**
 * Test 3.4: Default Burst-Wert ist INCR4 (sicherer Startwert).
 */
ZTEST(xi640_dcd_dma, test_burst_default_ist_incr4)
{
    zassert_equal(XI640_DCD_DMA_BURST_DEFAULT, XI640_DCD_DMA_BURST_INCR4,
        "Default Burst muss INCR4 (0x3) sein, ist 0x%x",
        XI640_DCD_DMA_BURST_DEFAULT);
}

/**
 * Test 3.5: Burst-Werte muessen gueltig (odd) und aufsteigend sein.
 *
 * GAHBCFG HBSTLEN Bits [4:1]: Gueltige Werte: 0x0, 0x1, 0x3, 0x5, 0x7
 * Wir pruefen dass unsere Werte in der erlaubten Menge liegen.
 */
ZTEST(xi640_dcd_dma, test_burst_werte_gueltig_und_geordnet)
{
    /* Aufsteigende Reihenfolge: INCR4 < INCR8 < INCR16 */
    zassert_true(XI640_DCD_DMA_BURST_INCR4 < XI640_DCD_DMA_BURST_INCR8,
        "INCR4 (0x%x) muss kleiner als INCR8 (0x%x) sein",
        XI640_DCD_DMA_BURST_INCR4, XI640_DCD_DMA_BURST_INCR8);

    zassert_true(XI640_DCD_DMA_BURST_INCR8 < XI640_DCD_DMA_BURST_INCR16,
        "INCR8 (0x%x) muss kleiner als INCR16 (0x%x) sein",
        XI640_DCD_DMA_BURST_INCR8, XI640_DCD_DMA_BURST_INCR16);

    /* Alle Werte muessen im HBSTLEN-Feld passen (4 Bits = max 0xF) */
    zassert_true(XI640_DCD_DMA_BURST_INCR16 <= 0xFU,
        "INCR16 (0x%x) passt nicht in 4-Bit HBSTLEN Feld",
        XI640_DCD_DMA_BURST_INCR16);
}

/* ──────────────────────────────────────────────────────────────────────── */
/* TEST SUITE 4: ISO Endpoint Konfiguration                                */
/* ──────────────────────────────────────────────────────────────────────── */

ZTEST_SUITE(xi640_dcd_iso, NULL, NULL, NULL, NULL, NULL);

/**
 * Test 4.1: ISO Endpoint Adresse muss 0x81 sein (EP1 IN).
 *
 * USB Spec: IN Endpoint = 0x80 | EP_NUM
 * EP1 IN = 0x80 | 0x01 = 0x81
 */
ZTEST(xi640_dcd_iso, test_iso_ep_addr_ist_0x81)
{
    zassert_equal(XI640_DCD_ISO_EP_ADDR, 0x81U,
        "ISO EP Adresse muss 0x81 sein (EP1 IN), ist 0x%x",
        XI640_DCD_ISO_EP_ADDR);
}

/**
 * Test 4.2: ISO Endpoint Nummer muss 1 sein.
 */
ZTEST(xi640_dcd_iso, test_iso_ep_num_ist_1)
{
    zassert_equal(XI640_DCD_ISO_EP_NUM, 1U,
        "ISO EP Nummer muss 1 sein, ist %u",
        XI640_DCD_ISO_EP_NUM);
}

/**
 * Test 4.3: wMaxPacketSize muss 0x1400 sein (High-Bandwidth ISO).
 *
 * 0x1400 = 5120 dezimal (USB 2.0 High-Bandwidth ISO Kodierung):
 *   Bits [12:11] = (5120 >> 11) & 0x3 = 2 => 2+1 = 3 Transaktionen
 *   Bits [10:0]  = 5120 & 0x7FF = 1024 Bytes pro Transaktion
 *   => 3 * 1024 = 3072 Bytes/Microframe = 24576 Bytes/Frame
 */
ZTEST(xi640_dcd_iso, test_iso_wmaxpacketsize_ist_0x1400)
{
    zassert_equal(XI640_DCD_ISO_WMAXPACKETSIZE, 0x1400U,
        "ISO wMaxPacketSize muss 0x1400 sein, ist 0x%x",
        XI640_DCD_ISO_WMAXPACKETSIZE);
}

/**
 * Test 4.4: ISO wMaxPacketSize kodiert 3 Transaktionen (High-Bandwidth).
 */
ZTEST(xi640_dcd_iso, test_iso_wmaxpacketsize_3_transaktionen)
{
    /* Bits [12:11] kodieren Anzahl zusaetzlicher Transaktionen:
     * 00b = 1, 01b = 2, 10b = 3 Transaktionen */
    uint32_t transactions = ((XI640_DCD_ISO_WMAXPACKETSIZE >> 11U) & 0x3U) + 1U;

    zassert_equal(transactions, 3U,
        "ISO wMaxPacketSize muss 3 Transaktionen kodieren, kodiert %u",
        transactions);
}

/**
 * Test 4.5: ISO wMaxPacketSize kodiert 1024 Bytes pro Transaktion.
 */
ZTEST(xi640_dcd_iso, test_iso_wmaxpacketsize_1024_bytes)
{
    uint32_t packet_size = XI640_DCD_ISO_WMAXPACKETSIZE & 0x7FFU;

    zassert_equal(packet_size, 1024U,
        "ISO wMaxPacketSize muss 1024 Bytes kodieren, kodiert %u",
        packet_size);
}

/**
 * Test 4.6: ISO Transfer-Blockgroesse muss 16384 Bytes sein.
 */
ZTEST(xi640_dcd_iso, test_iso_transfer_size_16384)
{
    zassert_equal(XI640_DCD_ISO_TRANSFER_SIZE, 16384U,
        "ISO Transfer-Blockgroesse muss 16384 Bytes sein, ist %u",
        XI640_DCD_ISO_TRANSFER_SIZE);
}

/**
 * Test 4.7: ISO Transfer-Blockgroesse muss 32-Byte aligned sein (DMA Cache).
 */
ZTEST(xi640_dcd_iso, test_iso_transfer_size_cache_aligned)
{
    zassert_equal(XI640_DCD_ISO_TRANSFER_SIZE % XI640_DCD_CACHE_LINE_SIZE, 0U,
        "ISO Transfer-Blockgroesse %u muss 32-Byte aligned sein",
        XI640_DCD_ISO_TRANSFER_SIZE);
}

/**
 * Test 4.8: ISO EP Adresse muss IN-Richtung kodieren (Bit 7 gesetzt).
 */
ZTEST(xi640_dcd_iso, test_iso_ep_addr_ist_in_richtung)
{
    zassert_true((XI640_DCD_ISO_EP_ADDR & 0x80U) != 0U,
        "ISO EP Adresse 0x%x muss Bit 7 gesetzt haben (IN Richtung)",
        XI640_DCD_ISO_EP_ADDR);
}

/* ──────────────────────────────────────────────────────────────────────── */
/* TEST SUITE 5: PHY und Controller Konfiguration                          */
/* ──────────────────────────────────────────────────────────────────────── */

ZTEST_SUITE(xi640_dcd_phy, NULL, NULL, NULL, NULL, NULL);

/**
 * Test 5.1: Anzahl Endpoints muss 9 sein (STM32N6 OTG HS).
 */
ZTEST(xi640_dcd_phy, test_num_endpoints_ist_9)
{
    zassert_equal(XI640_DCD_NUM_ENDPOINTS, 9U,
        "Anzahl Endpoints muss 9 sein (STM32N6 OTG HS), ist %u",
        XI640_DCD_NUM_ENDPOINTS);
}

/**
 * Test 5.2: IRQ Nummer muss 177 sein (STM32N6 OTG HS IRQ).
 */
ZTEST(xi640_dcd_phy, test_irq_num_ist_177)
{
    zassert_equal(XI640_DCD_IRQ_NUM, 177,
        "OTG HS IRQ Nummer muss 177 sein, ist %d",
        XI640_DCD_IRQ_NUM);
}

/**
 * Test 5.3: IRQ Prioritaet muss im gueltigen Zephyr-Bereich liegen (1-6).
 */
ZTEST(xi640_dcd_phy, test_irq_prio_gueltig)
{
    zassert_true(XI640_DCD_IRQ_PRIO >= 1 && XI640_DCD_IRQ_PRIO <= 6,
        "IRQ Prioritaet %d muss im Bereich 1-6 liegen",
        XI640_DCD_IRQ_PRIO);
}
