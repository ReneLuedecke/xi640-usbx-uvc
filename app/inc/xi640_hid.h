/**
 * @file xi640_hid.h
 * @brief Xi640 HID-Interface — Vendor Usage Page 0xFFA0, kompatibel mit
 *        Optris PIX Connect / IRImager SDK HID-Protokoll.
 *
 * Kommando-Format (Output Report, 40 Bytes):
 *   Byte 0: Geräte-Klasse (XI640_CMD_CYP = 0x34)
 *   Byte 1: Kommando-ID
 *   Byte 2-39: Parameter (kommandoabhängig)
 *
 * Response-Format (Input Report, 64 Bytes):
 *   Housekeeping-Daten (Temperaturen, FW-Version, HZ-Modus)
 */

#ifndef XI640_HID_H_
#define XI640_HID_H_

#include <stdint.h>

/* ── Geräte-Klassen ─────────────────────────────────────────────────────── */
#define XI640_CMD_CYP           0x34U   /* Cypress FX2LP-kompatibel */
#define XI640_CMD_MSP           0x4DU   /* MSP430-Klasse (nicht implementiert) */

/* ── Kommando-IDs (Byte 1) ──────────────────────────────────────────────── */
#define XI640_CMD_HKD_GET       0x03U   /* Housekeeping-Daten abfragen */
#define XI640_CMD_PW_CONTROL    0x40U   /* Power Control */
#define XI640_CMD_FW_CYP_VERS   0x42U   /* Cypress FW-Version abfragen */
#define XI640_CMD_FW_MSP_VERS   0x43U   /* MSP FW-Version abfragen */
#define XI640_CMD_RD_EEPROM     0x50U   /* EEPROM lesen */
#define XI640_CMD_WR_EEPROM     0x51U   /* EEPROM schreiben */
#define XI640_CMD_FLAG_DIR      0x55U   /* Flag/Shutter Richtung */
#define XI640_CMD_DAC_SET       0x58U   /* DAC-Wert setzen (GFID / GSK) */
#define XI640_CMD_FLAG_CYCLE    0x65U   /* Flag-Zyklus starten */

/* ── DAC Register-Adressen (Byte 2 bei XI640_CMD_DAC_SET) ──────────────── */
#define XI640_DAC_GFID          0x10U   /* ATTO640D DAC_GFID (Reg 0x004B) */
#define XI640_DAC_GSK           0x12U   /* ATTO640D DAC_GSK (Reg 0x004C/D) */

/* ── Report-Größen ──────────────────────────────────────────────────────── */
#define XI640_HID_IN_SIZE       64U     /* Input Report  (Device → Host) */
#define XI640_HID_OUT_SIZE      40U     /* Output Report (Host → Device) */

/* ── Housekeeping-Response Byte-Offsets ─────────────────────────────────── */
#define XI640_HKD_TEMP_MSB      12U     /* Gehäusetemperatur MSB (Byte 12) */
#define XI640_HKD_TEMP_LSB      13U     /* Gehäusetemperatur LSB (Byte 13) */
#define XI640_HKD_FW_LSB        15U     /* FW-Version LSB (Byte 15) */
#define XI640_HKD_FW_MSB        16U     /* FW-Version MSB (Byte 16) */
#define XI640_HKD_FLAGS         19U     /* Flags Byte 19: Bit5=HZ9 (1=100Hz) */

/**
 * @brief HID-Interface initialisieren und beim Zephyr USB-Stack registrieren.
 *
 * MUSS vor sample_usbd_init_device() aufgerufen werden — hid_device_register()
 * muss vor usbd_enable() abgeschlossen sein.
 *
 * @return 0 bei Erfolg, negativer Fehlercode sonst.
 */
int xi640_hid_init(void);

/**
 * @brief Output Report (40 Bytes) verarbeiten, Input Report (64 Bytes) befüllen.
 *        Interne Funktion, auch für Unit-Tests zugänglich.
 *
 * @param out_report  Empfangener Output Report (40 Bytes, Byte 0=Klasse, Byte 1=Cmd)
 * @param in_report   Ziel-Puffer für Input Report (64 Bytes, wird befüllt)
 */
void xi640_hid_process_command(const uint8_t *out_report, uint8_t *in_report);

#endif /* XI640_HID_H_ */
