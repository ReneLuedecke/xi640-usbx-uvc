/**
 * @file lynred_atto640d.h
 * @brief Lynred ATTO640D-04 Bolometer-Initialisierung (Analog Mode)
 *
 * Steuert den FPA-Startup via:
 * - I2C1 (PH9=SCL, PC1=SDA) @ 0x12, 16-bit Register-Adressierung
 * - Master Clock 10 MHz via Zephyr PWM (TIM3_CH2, PC7, AF2)
 * - Power-Enable GPIOs: AVDD (PE8), DVDD (PE14)
 * - Reset GPIO: NRST (PC8)
 * - Frame-Trigger GPIO: SEQ_TRIG (PC7)
 *
 * Referenz: Lynred UG038, Table 33 (Startup), Table 34 (Shutdown)
 */

#ifndef LYNRED_ATTO640D_H_
#define LYNRED_ATTO640D_H_

#include <stdint.h>
#include <stdbool.h>

/* ===== Rückgabecodes ===== */
#define ATTO640D_OK            0
#define ATTO640D_ERR_I2C      -1
#define ATTO640D_ERR_ID       -2
#define ATTO640D_ERR_TIMEOUT  -3
#define ATTO640D_ERR_CLOCK    -4
#define ATTO640D_ERR_INIT     -5

/* ===== I2C-Adresse (7-bit) ===== */
#define ATTO640D_I2C_ADDR     0x12U  /* I2CAD-Pin auf GND → Adresse 0x12 */

/* ===== Master Clock ===== */
#define ATTO640D_MC_FREQ_HZ   33000000U  /* 33 MHz (Spec: 24–40 MHz) */
#define ATTO640D_MC_DUTY_PCT  75U         /* 75% (Spec: 75–85%) */

/* ===== Register-Adressen (16-bit, MSB first über I2C) ===== */
/* Seriennummer & Kalibrierung */
#define ATTO640D_REG_SERIAL_NB_A     0x0000U  /* S/N Byte A — NB1[12:8] */
#define ATTO640D_REG_SERIAL_NB_B     0x0001U  /* S/N Byte B — NB1[7:0] */
#define ATTO640D_REG_SERIAL_NB_C     0x0002U  /* S/N Byte C — NB2[4:0] + NB3[9:8] */
#define ATTO640D_REG_SERIAL_NB_D     0x0003U  /* S/N Byte D — NB3[7:0] */
#define ATTO640D_REG_VTEMP_D_OFFS_A  0x001DU  /* VTEMP_D_OFFSET High — Bits[5:0] = OFFSET[13:8] */
#define ATTO640D_REG_VTEMP_D_OFFS_B  0x001EU  /* VTEMP_D_OFFSET Low  — Bits[7:0] = OFFSET[7:0] */
/* Bild-Parameter */
#define ATTO640D_REG_GAIN_IMAGE      0x0040U  /* Gain[6:4], TRIGGER_1[7] */
#define ATTO640D_REG_DIGITAL_OUTPUT  0x0041U  /* TRIGGER_2[3] */
#define ATTO640D_REG_DAC_GFID        0x004BU  /* DAC GFID [7:0] — V = 0.011×val + 0.7806 V (Thomas: 0x77=2.09V) */
#define ATTO640D_REG_DAC_GSK_A       0x004CU  /* DAC GSK[9:8] (Thomas: 0x01) */
#define ATTO640D_REG_DAC_GSK_B       0x004DU  /* DAC GSK[7:0] (Thomas: 0x01 → GSK=1.99V) */
#define ATTO640D_REG_INTERLINE        0x0062U  /* Blanking-Takte am Zeilenende (Thomas: 0x1A) */
#define ATTO640D_REG_INTEGRATION_A   0x004FU  /* Integrationszeit High-Byte [1:0] */
#define ATTO640D_REG_INTEGRATION_B   0x0050U  /* Integrationszeit Low-Byte [7:0] */
#define ATTO640D_REG_INTERFRAME      0x0056U  /* Blanking-Takte am Frame-Ende (Thomas: 0x0A) */
/* Konfiguration */
#define ATTO640D_REG_CONFIG_A        0x005BU  /* Konfiguration A (Standby, PowerDown, VTEMP_EN) */
#define ATTO640D_REG_CONFIG_B        0x005CU  /* Konfiguration B (I2C_DIFF_EN[0], START_SEQ[2]) */
#define ATTO640D_REG_CONFIG_E        0x005EU  /* Konfiguration E (REG_INIT[1] / INTERNAL_POLAR[1]) */
#define ATTO640D_REG_CONFIG_C        0x0099U  /* Konfiguration C (Thomas: 0x40) */
/* Identifikation & Status */
#define ATTO640D_REG_ROIC_REV        0x00F6U  /* ROIC Revision */
#define ATTO640D_REG_READ_ONLY_A     0x00F7U  /* Identifikation — erwartet: 0x55 */
#define ATTO640D_REG_READ_ONLY_B     0x00F8U  /* Identifikation — erwartet: 0xC6 */
#define ATTO640D_REG_READ_ONLY_C     0x00F9U  /* Identifikation — erwartet: 0xCA */
#define ATTO640D_REG_STATUS          0x0063U  /* Status: ROIC_INIT_DONE[3], SEQ_STATUS[6] */

/* ===== Status-Bits ===== */
#define ATTO640D_STATUS_ROIC_INIT_DONE  (1U << 3)  /* 1 = ROIC initialisiert */
#define ATTO640D_STATUS_SEQ_STATUS      (1U << 6)  /* 0 = Sequencer läuft, 1 = gestoppt */

/* ===== Erwartete ID-Bytes ===== */
#define ATTO640D_ID_A  0x55U
#define ATTO640D_ID_B  0xC6U
#define ATTO640D_ID_C  0xCAU

/* ===== Public API ===== */

/**
 * @brief Führt die vollständige Analog-Mode-Startup-Sequenz durch (UG038 Table 33).
 * @return ATTO640D_OK bei Erfolg, negativer Fehlercode sonst.
 */
int atto640d_init(void);

/**
 * @brief Führt die Shutdown-Sequenz durch (UG038 Table 34).
 *        Schaltet MC, DVDD und AVDD in korrekter Reihenfolge ab.
 */
void atto640d_shutdown(void);

/**
 * @brief Startet den 24 MHz Master Clock (TIM3_CH1, PC6, AF2).
 * @return ATTO640D_OK bei Erfolg, ATTO640D_ERR_CLOCK wenn Frequenz ausserhalb Spec.
 */
int atto640d_start_mc(void);

/**
 * @brief Stoppt den Master Clock und setzt PC6 auf Low.
 */
void atto640d_stop_mc(void);
int  atto640d_read_id_regs(uint8_t *a, uint8_t *b, uint8_t *c);
int  atto640d_read_status_reg(uint8_t *val);

/**
 * @brief Passt DAC_GSK iterativ an, bis der mittlere ADC-Wert des
 *        aktuellen DCMIPP-Frames ~8000 LSB beträgt.
 *        Muss nach dcmipp_capture_init() aufgerufen werden.
 * @return 0 = konvergiert, -EAGAIN = nicht konvergiert, -ETIMEDOUT = kein Frame
 */
int atto640d_auto_level(void);

/**
 * @brief Liest Sensor-Info einmalig nach Init und loggt: S/N, ROIC Rev,
 *        Gain, Integrationszeit, VTEMP_D_OFFSET, CONFIG_A, STATUS.
 *        Berechnet FPA Junction-Temperatur aus VTEMP_P falls verfügbar.
 * @param vtemp_mv   VTEMP_P-Spannung in mV (vom ADS1110, nach Spannungsteiler-Kompensation).
 * @param vtemp_valid true wenn vtemp_mv gültig, false wenn ADS1110 nicht erreichbar.
 * @return ATTO640D_OK oder ATTO640D_ERR_I2C.
 */
int  atto640d_read_sensor_info(float vtemp_mv, bool vtemp_valid);

#endif /* LYNRED_ATTO640D_H_ */
