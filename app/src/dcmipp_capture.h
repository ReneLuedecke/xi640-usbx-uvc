/**
 * @file dcmipp_capture.h
 * @brief DCMIPP Parallel-Interface Capture (ATTO640D-04 → CPLD → AD9649 → STM32N6)
 *
 * Pipe0 (Dump Mode): 14-Bit Rohdaten in 16-Bit-Words, 640×480.
 * Frame-Buffer liegen in AXISRAM1 (@ 0x34000000, vor Code bei 0x34180400).
 */

#ifndef DCMIPP_CAPTURE_H_
#define DCMIPP_CAPTURE_H_

#include <stdint.h>

#define DCMIPP_FRAME_WIDTH   640
#define DCMIPP_FRAME_HEIGHT  480
#define DCMIPP_FRAME_BPP     2        /* 14-Bit in 16-Bit Container (uint16_t pro Pixel) */
#define DCMIPP_FRAME_SIZE    (DCMIPP_FRAME_WIDTH * DCMIPP_FRAME_HEIGHT * DCMIPP_FRAME_BPP)

/* MINIMAL-TEST: BYPASS_HAL=1, kein memcpy, kein PSRAM.
 * dcmipp_capture_get_frame() gibt Zeiger direkt auf AXISRAM-DMA-Buffer zurück. */

/**
 * @brief Initialisiert DCMIPP, setzt Format, startet Streaming (Double-Buffer).
 * @return 0 bei Erfolg, negativer Fehlercode sonst.
 */
int dcmipp_capture_init(void);

/**
 * @brief Wartet auf den nächsten fertigen Frame und gibt Zeiger zurück.
 *        D-Cache wird nach DMA-Schreibzugriff invalidiert.
 *        Buffer wird automatisch wieder in die Queue eingereiht.
 * @param data  Zeiger auf Frame-Daten (gültig bis nächster Aufruf)
 * @param size  Tatsächliche Bytes im Frame
 * @param timeout_ms Wartezeit in ms (0 = sofort, -1 = ewig)
 * @return 0 bei Erfolg, -ETIMEDOUT bei Timeout, negativer Fehlercode sonst.
 */
int dcmipp_capture_get_frame(uint8_t **data, uint32_t *size, int timeout_ms);

#endif /* DCMIPP_CAPTURE_H_ */
