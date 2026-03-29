/**
 * @file lynred_atto640d.c
 * @brief Lynred ATTO640D-04 Bolometer-Initialisierung (Analog Mode)
 *
 * Implementiert UG038 Table 33 (Startup) und Table 34 (Shutdown).
 *
 * Master Clock via Zephyr PWM-Treiber (TIM3_CH2/PC7/AF2, DTS-Node atto640d_mc_pwm).
 * I2C1 und GPIOs nutzen die Zephyr Device API.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/cache.h>
#include <soc.h>  /* STM32N6 CMSIS-Header, HAL-Includes */

#include "lynred_atto640d.h"
#include "dcmipp_capture.h"

LOG_MODULE_REGISTER(atto640d, LOG_LEVEL_INF);

/* ===== I2C1 Device (PH9=SCL, PC1=SDA) ===== */
#define I2C1_NODE  DT_NODELABEL(i2c1)
static const struct device *i2c1_dev;

/* ===== PWM Master Clock (TIM3_CH2, PC7, AF2) ===== */
#define MC_PWM_NODE  DT_NODELABEL(atto640d_mc_pwm)
static const struct device *mc_pwm_dev = DEVICE_DT_GET(MC_PWM_NODE);
#define MC_PWM_CHANNEL  2U  /* TIM3_CH2 */

/* ===== GPIO-Specs aus Device Tree ===== */
static const struct gpio_dt_spec nrst_pin =
    GPIO_DT_SPEC_GET(DT_NODELABEL(atto640d_nrst), gpios);
static const struct gpio_dt_spec seq_trig_pin =
    GPIO_DT_SPEC_GET(DT_NODELABEL(atto640d_seq_trig), gpios);
static const struct gpio_dt_spec avdd_en_pin =
    GPIO_DT_SPEC_GET(DT_NODELABEL(atto640d_avdd_en), gpios);
static const struct gpio_dt_spec vdda_en_pin =
    GPIO_DT_SPEC_GET(DT_NODELABEL(atto640d_vdda_en), gpios);

/* ===== I2C Helper: 16-bit Register-Adressierung ===== */

/**
 * @brief Schreibt 1 Byte in ein 16-bit-adressiertes Register.
 *
 * Protokoll: [reg_addr_MSB, reg_addr_LSB, value] — 3-Byte I2C Write.
 */
static int atto640d_i2c_write_reg(uint16_t reg_addr, uint8_t value)
{
    uint8_t buf[3] = {
        (uint8_t)(reg_addr >> 8),    /* MSB der Registeradresse */
        (uint8_t)(reg_addr & 0xFFU), /* LSB der Registeradresse */
        value
    };

    return i2c_write(i2c1_dev, buf, sizeof(buf), ATTO640D_I2C_ADDR);
}

/**
 * @brief Liest 1 Byte aus einem 16-bit-adressierten Register.
 *
 * Protokoll: Write 2 Byte Adresse → Repeated Start → Read 1 Byte.
 */
static int atto640d_i2c_read_reg(uint16_t reg_addr, uint8_t *value)
{
    uint8_t addr_buf[2] = {
        (uint8_t)(reg_addr >> 8),
        (uint8_t)(reg_addr & 0xFFU)
    };

    return i2c_write_read(i2c1_dev, ATTO640D_I2C_ADDR,
                          addr_buf, sizeof(addr_buf),
                          value, 1U);
}

/* ===== GPIO Init ===== */

static int atto640d_gpio_init(void)
{
    int ret;

    /* ===== RIF: PE9 und PE15 für Non-Secure/Non-Privileged freigeben ===== */
    /* FSBL läuft im Secure-Modus → Zugriff über Secure-Alias GPIOE_S.
     * Ohne diese Freigabe kann Zephyrs GPIO-Driver (Non-Secure Alias) die
     * Pins nicht steuern — ODR wird geschrieben, aber kein Pegel am Pin.
     * SECCFGR  Bit = 0 → Pin ist Non-Secure zugänglich
     * PRIVCFGR Bit = 0 → Pin ist Non-Privileged zugänglich
     */
    GPIO_TypeDef *gpioe_s = (GPIO_TypeDef *)(GPIOE_BASE_S);
    gpioe_s->SECCFGR  &= ~((1U << 9U) | (1U << 15U));
    gpioe_s->PRIVCFGR &= ~((1U << 9U) | (1U << 15U));
    LOG_INF("DEBUG RIF: GPIOE_S SECCFGR=0x%08x PRIVCFGR=0x%08x",
            gpioe_s->SECCFGR, gpioe_s->PRIVCFGR);

    if (!device_is_ready(nrst_pin.port)) {
        LOG_ERR("ATTO640D: NRST GPIO (PC8) nicht bereit");
        return ATTO640D_ERR_INIT;
    }
    if (!device_is_ready(avdd_en_pin.port)) {
        LOG_ERR("ATTO640D: AVDD_EN GPIO (PE8) nicht bereit");
        return ATTO640D_ERR_INIT;
    }
    if (!device_is_ready(vdda_en_pin.port)) {
        LOG_ERR("ATTO640D: VDDA_EN GPIO (PE14) nicht bereit");
        return ATTO640D_ERR_INIT;
    }
    if (!device_is_ready(seq_trig_pin.port)) {
        LOG_ERR("ATTO640D: SEQ_TRIG GPIO (PC7) nicht bereit");
        return ATTO640D_ERR_INIT;
    }

    /* Alle Outputs initial LOW (sicherer Zustand: Power aus, Reset aktiv) */
    ret = gpio_pin_configure_dt(&nrst_pin, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        LOG_ERR("ATTO640D: NRST config failed: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&avdd_en_pin, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        LOG_ERR("ATTO640D: AVDD_EN config failed: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&vdda_en_pin, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        LOG_ERR("ATTO640D: VDDA_EN config failed: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&seq_trig_pin, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        LOG_ERR("ATTO640D: SEQ_TRIG config failed: %d", ret);
        return ret;
    }

    /* ===== MODER forcieren: PE9 und PE15 explizit auf Output (0b01) ===== */
    /* Zephyr konfiguriert diese Pins ggf. falsch (Analog statt Output).
     * Direkte Register-Korrektur nach gpio_pin_configure_dt() als Absicherung.
     */
    GPIOE->MODER &= ~((0x3U << (9U * 2U)) | (0x3U << (15U * 2U)));
    GPIOE->MODER |=  ((0x1U << (9U * 2U)) | (0x1U << (15U * 2U)));

    /* ===== SICHERHEITSCHECK: ADC-Datenpins dürfen NIEMALS Output sein! ===== */
    /* PE0,PE1,PE3,PE4,PE5,PE7,PE8,PE10 = AD9649 ADC-Dateneingänge.
     * Output-Treiber gegen AD9649-Ausgang → Bus-Konflikt → ADC-IC wird heiß!
     * ADC-Pins: alle außer PE2, PE6, PE9 (AVDD-EN), PE11-PE15
     */
    {
        static const uint8_t adc_pins[] = {0, 1, 3, 4, 5, 7, 8, 10};
        uint32_t moder = GPIOE->MODER;
        bool conflict = false;

        for (int i = 0; i < (int)ARRAY_SIZE(adc_pins); i++) {
            uint8_t pin = adc_pins[i];
            uint32_t mode = (moder >> (pin * 2U)) & 0x3U;

            if (mode == 0x1U) {  /* Output — BUS-KONFLIKT! */
                LOG_ERR("!!! GEFAHR: PE%u als Output — ADC Bus-Konflikt! Setze auf Analog.", pin);
                GPIOE->MODER |= (0x3U << (pin * 2U));  /* Force Analog (0b11) */
                conflict = true;
            }
        }

        if (conflict) {
            LOG_ERR("ADC-Pin-Konflikt erkannt und korrigiert — AD9649 auf Beschaedigung prüfen!");
        }
    }

    /* ===== Verifikation: finaler MODER-Zustand ===== */
    LOG_INF("ATTO640D: GPIOs bereit (NRST=PC9, SEQ_TRIG=PC8, AVDD=PE9, DVDD=PE15)");
    LOG_INF("DEBUG GPIOE MODER final: 0x%08x  PE9=%s  PE15=%s",
            GPIOE->MODER,
            ((GPIOE->MODER >> (9U * 2U)) & 0x3U) == 0x1U ? "Output OK" : "FEHLER",
            ((GPIOE->MODER >> (15U * 2U)) & 0x3U) == 0x1U ? "Output OK" : "FEHLER");
    LOG_INF("DEBUG gpio_init: GPIOE ODR=0x%08x OTYPER=0x%04x", GPIOE->ODR, GPIOE->OTYPER);
    LOG_INF("DEBUG gpio_init: RCC AHB4ENR=0x%08x", RCC->AHB4ENR);
    return 0;
}

/* ===== Master Clock: TIM3_CH2 @ PC7 via Zephyr PWM-Treiber ===== */

/**
 * @brief Startet den Master Clock via TIM3_CH2 (PC7, AF2) — Zephyr PWM API.
 *
 * PC7/AF2/pinctrl wird vom Zephyr-Treiber verwaltet (DTS: tim3_ch2_pc7).
 * Kein direkter Register-Zugriff nötig — kein GPIOC AFR[0]-Konflikt mehr.
 */
int atto640d_start_mc(void)
{
    if (!device_is_ready(mc_pwm_dev)) {
        LOG_ERR("ATTO640D MC: PWM-Device (TIM3_CH2/PC7) nicht bereit");
        return ATTO640D_ERR_CLOCK;
    }

    uint32_t period_ns = NSEC_PER_SEC / ATTO640D_MC_FREQ_HZ;
    uint32_t pulse_ns  = (period_ns * ATTO640D_MC_DUTY_PCT) / 100U;

    int ret = pwm_set(mc_pwm_dev, MC_PWM_CHANNEL, period_ns, pulse_ns,
                      PWM_POLARITY_NORMAL);
    if (ret != 0) {
        LOG_ERR("ATTO640D MC: pwm_set fehlgeschlagen: %d", ret);
        return ATTO640D_ERR_CLOCK;
    }

    /* Spec-Check: F_MC = 24–40 MHz (UG038) — aktuell DEBUG-Modus */
    if (ATTO640D_MC_FREQ_HZ > 40000000U) {
        LOG_WRN("ATTO640D MC: %u Hz oberhalb Spec-Maximum (40 MHz)!", ATTO640D_MC_FREQ_HZ);
    } else if (ATTO640D_MC_FREQ_HZ < 24000000U) {
        LOG_WRN("ATTO640D MC: %u Hz unterhalb Spec (24 MHz) — DEBUG-Modus", ATTO640D_MC_FREQ_HZ);
    }

    LOG_INF("ATTO640D MC: TIM3_CH2 (PC7) gestartet — %u Hz, %u%% Duty",
            ATTO640D_MC_FREQ_HZ, ATTO640D_MC_DUTY_PCT);
    return ATTO640D_OK;
}

/**
 * @brief Stoppt den Master Clock (Puls = 0 → PC7 Low).
 */
void atto640d_stop_mc(void)
{
    uint32_t period_ns = NSEC_PER_SEC / ATTO640D_MC_FREQ_HZ;

    /* Puls auf 0 setzen → PC7 bleibt Low (PWM-Treiber hält Pin im definierten Zustand) */
    pwm_set(mc_pwm_dev, MC_PWM_CHANNEL, period_ns, 0U, PWM_POLARITY_NORMAL);
    LOG_INF("ATTO640D MC: TIM3 gestoppt, PC7 = Low");
}

/* ===== Startup-Sequenz (UG038 Table 33) ===== */

int atto640d_init(void)
{
    int ret;
    uint8_t id_a, id_b, id_c;

    LOG_INF("ATTO640D: Startup-Sequenz (Analog Mode, UG038 Table 33)");

    /* I2C1 Device holen und prüfen */
    i2c1_dev = DEVICE_DT_GET(I2C1_NODE);
    if (!device_is_ready(i2c1_dev)) {
        LOG_ERR("ATTO640D: I2C1 nicht bereit (PH9/PC1) — DTS aktiviert?");
        return ATTO640D_ERR_INIT;
    }
    LOG_INF("ATTO640D: I2C1 bereit (PH9=SCL, PC1=SDA, 400 kHz)");

    /* ===== I2C1 Pins manuell als AF4 Open-Drain konfigurieren ===== */
    /* Zephyr/pinctrl konfiguriert PH9/PC1 ggf. nicht korrekt für STM32N6.
     * I2C MUSS Open-Drain sein (nicht Push-Pull)!
     * AF4 = I2C1 auf PH9 (SCL) und PC1 (SDA) — STM32-Standard.
     * HAL_GPIO_Init() nicht in Zephyr verfügbar → direkte Register-Manipulation.
     */

    /* PH9 = I2C1_SCL: AF4, Open-Drain, Pull-up, High Speed */
    __HAL_RCC_GPIOH_CLK_ENABLE();
    GPIOH->MODER   &= ~(0x3U << (9U * 2U));
    GPIOH->MODER   |=  (0x2U << (9U * 2U));   /* Alternate Function */
    GPIOH->OTYPER  |=  (1U << 9U);             /* Open-Drain */
    GPIOH->OSPEEDR &= ~(0x3U << (9U * 2U));
    GPIOH->OSPEEDR |=  (0x2U << (9U * 2U));   /* High Speed */
    GPIOH->PUPDR   &= ~(0x3U << (9U * 2U));
    GPIOH->PUPDR   |=  (0x1U << (9U * 2U));   /* Pull-up */
    GPIOH->AFR[1]  &= ~(0xFU << ((9U - 8U) * 4U));
    GPIOH->AFR[1]  |=  (4U   << ((9U - 8U) * 4U));  /* AF4 = I2C1 */

    /* PC1 = I2C1_SDA: AF4, Open-Drain, Pull-up, High Speed */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIOC->MODER   &= ~(0x3U << (1U * 2U));
    GPIOC->MODER   |=  (0x2U << (1U * 2U));   /* Alternate Function */
    GPIOC->OTYPER  |=  (1U << 1U);             /* Open-Drain */
    GPIOC->OSPEEDR &= ~(0x3U << (1U * 2U));
    GPIOC->OSPEEDR |=  (0x2U << (1U * 2U));   /* High Speed */
    GPIOC->PUPDR   &= ~(0x3U << (1U * 2U));
    GPIOC->PUPDR   |=  (0x1U << (1U * 2U));   /* Pull-up */
    GPIOC->AFR[0]  &= ~(0xFU << (1U * 4U));
    GPIOC->AFR[0]  |=  (4U   << (1U * 4U));   /* AF4 = I2C1 */

    /* GPIOs initialisieren */
    ret = atto640d_gpio_init();
    if (ret != 0) {
        return ret;
    }

    /* Step 1: NRST = 0 (Reset aktiv halten) */
    LOG_INF("ATTO640D: Step  1 — NRST = 0 (Reset aktiv)");
    gpio_pin_set_dt(&nrst_pin, 0);

    /* Step 2: AVDD einschalten (PE9 high → ADP7118: 3.75V) */
    LOG_INF("ATTO640D: Step  2 — AVDD-EN = 1 (3.75V Regler ein, IC370)");
    gpio_pin_set_dt(&avdd_en_pin, 1);
    LOG_INF("DEBUG Step 2: GPIOE MODER=0x%08x ODR=0x%08x OTYPER=0x%04x",
            GPIOE->MODER, GPIOE->ODR, GPIOE->OTYPER);

    /* Step 3: ≥ 10 µs warten (AVDD Anlaufzeit) */
    LOG_INF("ATTO640D: Step  3 — Warte 10 µs");
    k_busy_wait(10U);  /* k_busy_wait für µs-genaues Timing */

    /* Step 4: DVDD einschalten (PE15 high → ADP150: 1.8V) */
    LOG_INF("ATTO640D: Step  4 — VDDA-EN = 1 (1.8V Regler ein, IC360)");
    gpio_pin_set_dt(&vdda_en_pin, 1);
    LOG_INF("DEBUG Step 4: GPIOE MODER=0x%08x ODR=0x%08x OTYPER=0x%04x",
            GPIOE->MODER, GPIOE->ODR, GPIOE->OTYPER);

    /* Step 5: Power-Stabilisierung (AVDD > 3V, DVDD > 1.5V) */
    LOG_INF("ATTO640D: Step  5 — Warte 10 ms (Power-Stabilisierung)");
    k_sleep(K_MSEC(10));

    /* Step 6: Master Clock starten (TIM3_CH2/PC7 via PWM-Treiber) */
    LOG_INF("ATTO640D: Step  6 — Master Clock starten (%u Hz, %u%% Duty)",
            ATTO640D_MC_FREQ_HZ, ATTO640D_MC_DUTY_PCT);
    ret = atto640d_start_mc();
    if (ret != ATTO640D_OK) {
        LOG_ERR("ATTO640D: MC Start fehlgeschlagen (%d) — Shutdown", ret);
        atto640d_shutdown();
        return ret;
    }

    /* Step 7: NRST = 1 (Reset aufheben) */
    LOG_INF("ATTO640D: Step  7 — NRST = 1 (Reset aufgehoben)");
    gpio_pin_set_dt(&nrst_pin, 1);

    /* Step 8: ≥ 1.6 ms warten (FPA-Bootzeit) */
    LOG_INF("ATTO640D: Step  8 — Warte 2 ms (FPA Bootzeit ≥1.6 ms)");
    k_sleep(K_MSEC(2));

    /* Step 9: I2C Identifikation prüfen */
    LOG_INF("ATTO640D: Step  9 — I2C Identifikation (0x%04X/0x%04X/0x%04X)",
            ATTO640D_REG_READ_ONLY_A, ATTO640D_REG_READ_ONLY_B, ATTO640D_REG_READ_ONLY_C);

    ret = atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_A, &id_a);
    if (ret != 0) {
        LOG_ERR("ATTO640D: I2C Fehler READ_ONLY_A: %d — Power+MC bleiben an!", ret);
        return ATTO640D_ERR_I2C;
    }

    ret = atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_B, &id_b);
    if (ret != 0) {
        LOG_ERR("ATTO640D: I2C Fehler READ_ONLY_B: %d — Power+MC bleiben an!", ret);
        return ATTO640D_ERR_I2C;
    }

    ret = atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_C, &id_c);
    if (ret != 0) {
        LOG_ERR("ATTO640D: I2C Fehler READ_ONLY_C: %d — Power+MC bleiben an!", ret);
        return ATTO640D_ERR_I2C;
    }

    LOG_INF("ATTO640D: ID: A=0x%02X (erw. 0x%02X), B=0x%02X (erw. 0x%02X), C=0x%02X (erw. 0x%02X)",
            id_a, ATTO640D_ID_A, id_b, ATTO640D_ID_B, id_c, ATTO640D_ID_C);

    if (id_a != ATTO640D_ID_A || id_b != ATTO640D_ID_B || id_c != ATTO640D_ID_C) {
        LOG_ERR("ATTO640D: ID-Mismatch — FPA nicht erkannt oder MC nicht aktiv! Power+MC bleiben an!");
        return ATTO640D_ERR_ID;
    }
    LOG_INF("ATTO640D: ID OK — ATTO640D-04 erkannt");

    /* Step 10: CONFIG_B = 0x01 — I2C_DIFF_EN (Register-Änderungen an Sequencer weitergeben) */
    LOG_INF("ATTO640D: Step 10 — CONFIG_B = 0x01 (I2C_DIFF_EN)");
    ret = atto640d_i2c_write_reg(ATTO640D_REG_CONFIG_B, 0x01U);
    if (ret != 0) {
        LOG_ERR("ATTO640D: Step 10 CONFIG_B Write fehlgeschlagen: %d", ret);
        atto640d_shutdown();
        return ATTO640D_ERR_I2C;
    }

    /* Step 11: CONFIG_E = 0x02 — REG_INIT (Register initialisieren) */
    LOG_INF("ATTO640D: Step 11 — CONFIG_E = 0x02 (REG_INIT)");
    ret = atto640d_i2c_write_reg(ATTO640D_REG_CONFIG_E, 0x02U);
    if (ret != 0) {
        LOG_ERR("ATTO640D: Step 11 CONFIG_E Write fehlgeschlagen: %d", ret);
        atto640d_shutdown();
        return ATTO640D_ERR_I2C;
    }

    /* Step 12: ≥ 1.6 ms warten */
    LOG_INF("ATTO640D: Step 12 — Warte 2 ms");
    k_sleep(K_MSEC(2));

    /* Step 13: ≥ 300 ms warten (FPA Initialisierung, I2C-Programmierung erlaubt) */
    LOG_INF("ATTO640D: Step 13 — Warte 300 ms (FPA Initialisierung)");
    k_sleep(K_MSEC(300));


    /* INTERLINE (0x0062) = 0x1A: Blanking-Takte am Zeilenende 
    ret = atto640d_i2c_write_reg(ATTO640D_REG_INTERLINE, 0x1AU);
    if (ret != 0) {
        LOG_WRN("ATTO640D: INTERLINE Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: INTERLINE (0x0062) = 0x1A");*/

    /* INTERFRAME (0x0056) = 0x0A: Blanking-Takte am Frame-Ende 
    ret = atto640d_i2c_write_reg(ATTO640D_REG_INTERFRAME, 0x0AU);
    if (ret != 0) {
        LOG_WRN("ATTO640D: INTERFRAME Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: INTERFRAME (0x0056) = 0x0A");*/

    /* DIGITAL_OUTPUT (0x0041) = 0x40: ADC_EN=0 (externer AD9649 auf dem Board) */
    ret = atto640d_i2c_write_reg(ATTO640D_REG_DIGITAL_OUTPUT, 0x40U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: DIGITAL_OUTPUT Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: DIGITAL_OUTPUT (0x0041) = 0x40 (ADC_EN=0, externer AD9649 aktiv)");

   /* DAC_GFID (0x004B) = 0x77: V = 0.011×0x50 + 0.7806 = 2.09 V 
    ret = atto640d_i2c_write_reg(ATTO640D_REG_DAC_GFID, 0x90U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: DAC_GFID Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: DAC_GFID (0x004B) = 0x77 (2.09 V)");*/

    /* DAC_GSK_A (0x004C) = 0x01 
    ret = atto640d_i2c_write_reg(ATTO640D_REG_DAC_GSK_A, 0x01U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: DAC_GSK_A Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: DAC_GSK_A (0x004C) = 0x01");*/

    /* DAC_GSK_B (0x004D) = 0x01 (GSK = 1.99 V) 
    ret = atto640d_i2c_write_reg(ATTO640D_REG_DAC_GSK_B, 0x01U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: DAC_GSK_B Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: DAC_GSK_B (0x004D) = 0x01 (GSK=1.99 V)");*/

    /* CONFIG_C (0x0099) = 0x40 */
    ret = atto640d_i2c_write_reg(ATTO640D_REG_CONFIG_C, 0x40U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: CONFIG_C Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: CONFIG_C (0x0099) = 0x40");

    /* CONFIG_B = 0x05: START_SEQ[2] + I2C_DIFF_EN[0]
     * Übergibt alle obigen Register-Änderungen an den Sequencer und startet ihn. */
    ret = atto640d_i2c_write_reg(ATTO640D_REG_CONFIG_B, 0x05U);
    if (ret != 0) {
        LOG_ERR("ATTO640D: CONFIG_B (START_SEQ) Write fehlgeschlagen: %d", ret);
        atto640d_shutdown();
        return ATTO640D_ERR_I2C;
    }
    LOG_INF("ATTO640D: CONFIG_B (0x005C) = 0x05 (START_SEQ + I2C_DIFF_EN)");

    /* CONFIG_E / INTERNAL_POLAR (0x005E) = 0x02: nach Sequencer-Start */
    ret = atto640d_i2c_write_reg(ATTO640D_REG_CONFIG_E, 0x02U);
    if (ret != 0) {
        LOG_WRN("ATTO640D: CONFIG_E/INTERNAL_POLAR Write fehlgeschlagen: %d", ret);
    }
    LOG_INF("ATTO640D: CONFIG_E/INTERNAL_POLAR (0x005E) = 0x02");

    /* 14d: kurz warten (Sequencer-Start) */
    k_sleep(K_MSEC(10));

    /* 14e: STATUS prüfen — ROIC_INIT_DONE[3]=1, SEQ_STATUS[6]=0 (läuft) */
    {
        uint8_t status_val;

        ret = atto640d_i2c_read_reg(ATTO640D_REG_STATUS, &status_val);
        if (ret != 0) {
            LOG_ERR("ATTO640D: Step 14e STATUS Read fehlgeschlagen: %d", ret);
            atto640d_shutdown();
            return ATTO640D_ERR_I2C;
        }
        LOG_INF("ATTO640D: Step 14e STATUS=0x%02X (ROIC_INIT_DONE=%u, SEQ_STATUS=%u)",
                status_val,
                (status_val & ATTO640D_STATUS_ROIC_INIT_DONE) ? 1U : 0U,
                (status_val & ATTO640D_STATUS_SEQ_STATUS)     ? 1U : 0U);

        if (status_val & ATTO640D_STATUS_SEQ_STATUS) {
            LOG_ERR("ATTO640D: Sequencer nicht gestartet (SEQ_STATUS=1) — Startup fehlgeschlagen!");
            atto640d_shutdown();
            return ATTO640D_ERR_INIT;
        }
    }

    LOG_INF("ATTO640D: Startup abgeschlossen — Sequencer läuft, Free-run Modus aktiv");

    /* === Kalibrierungswerte GFID und GSK auslesen (UG038 Table 28) === */
    {
        uint8_t gfid_val, gsk_a, gsk_b;
        int r;

        r  = atto640d_i2c_read_reg(ATTO640D_REG_DAC_GFID,   &gfid_val);
        r |= atto640d_i2c_read_reg(ATTO640D_REG_DAC_GSK_A,  &gsk_a);
        r |= atto640d_i2c_read_reg(ATTO640D_REG_DAC_GSK_B,  &gsk_b);

        if (r == 0) {
            uint16_t gsk_raw = (((uint16_t)(gsk_a & 0x03U)) << 8U) | gsk_b;
            /* Spannungsformeln aus UG038:
             *   GFID (V) = 0.011 × DAC_GFID + 0.7806
             *   GSK  (V) = 0.0014 × DAC_GSK  + 1.6321
             * Fixpoint mit 4 Nachkommastellen (×10000) um float zu vermeiden.
             */
            uint32_t gfid_mv = (11U * gfid_val + 78U);  /* centiVolt → /100 = V */
            /* Für Log: mV-Näherung */
            uint32_t gfid_mv_full = (uint32_t)(11U * gfid_val) + 781U;
            uint32_t gsk_mv_full  = (uint32_t)(14U * gsk_raw + 16321U) / 10U;

            LOG_INF("ATTO640D Kalibrierung (UG038 Table 28):");
            LOG_INF("  DAC_GFID: 0x%02X (%u)  → GFID ≈ %u.%03u V",
                    gfid_val, gfid_val,
                    gfid_mv_full / 1000U, gfid_mv_full % 1000U);
            LOG_INF("  DAC_GSK:  0x%03X (%u)  → GSK  ≈ %u.%03u V",
                    gsk_raw, gsk_raw,
                    gsk_mv_full / 1000U, gsk_mv_full % 1000U);
            (void)gfid_mv;
        } else {
            LOG_WRN("ATTO640D: GFID/GSK auslesen fehlgeschlagen: %d", r);
        }
    }

    k_sleep(K_MSEC(500));  /* Warten auf stabile Video-Ausgabe */

    return ATTO640D_OK;
}

/* ===== Automatische GSK-Pegelregelung ===== */

/* Ziel-ADC-Mittelwert (halbe 14-Bit-Skala) und Toleranz */
#define AUTO_LEVEL_TARGET    8000U
#define AUTO_LEVEL_TOL        200U
#define AUTO_LEVEL_MAX_ITER    40

/*
 * atto640d_auto_level() — Passt DAC_GSK iterativ an, bis der mittlere
 * ADC-Wert des aktuellen DCMIPP-Frames im Bereich
 * [AUTO_LEVEL_TARGET ± AUTO_LEVEL_TOL] liegt.
 *
 * Richtungskonvention (UG038, Analog Mode, AD9649-Ausgang):
 *   GSK höher → ADC-Mittelwert steigt  (weniger Verstärkung)
 *   GSK niedriger → ADC-Mittelwert sinkt (mehr Verstärkung)
 * Falls Richtung am Board invertiert ist → Vorzeichen von 'step' tauschen.
 *
 * Rückgabe: 0 = konvergiert, -ETIMEDOUT = kein Frame, -EAGAIN = nicht konvergiert
 */
int atto640d_auto_level(void)
{
    uint8_t gsk_a, gsk_b;
    int ret;

    ret  = atto640d_i2c_read_reg(ATTO640D_REG_DAC_GSK_A, &gsk_a);
    ret |= atto640d_i2c_read_reg(ATTO640D_REG_DAC_GSK_B, &gsk_b);
    if (ret) {
        LOG_ERR("Auto-Level: GSK lesen fehlgeschlagen: %d", ret);
        return -EIO;
    }

    /* GSK als 10-Bit-Ganzzahl: A=Bits[9:8], B=Bits[7:0] (UG038 Table 28) */
    uint16_t gsk = (((uint16_t)(gsk_a & 0x03U)) << 8U) | gsk_b;
    LOG_INF("Auto-Level: Start GSK=0x%03X (%u), Ziel AVG=%u +/-%u",
            gsk, gsk, AUTO_LEVEL_TARGET, AUTO_LEVEL_TOL);

    for (int iter = 0; iter < AUTO_LEVEL_MAX_ITER; iter++) {
        /* Schritt 1: Aktuellen GSK-Wert schreiben
         * (Iteration 0: schreibt Ausgangswert ohne Änderung → saubere Basismessung) */
        gsk_a = (uint8_t)((gsk >> 8U) & 0x03U);
        gsk_b = (uint8_t)(gsk & 0xFFU);
        ret  = atto640d_i2c_write_reg(ATTO640D_REG_DAC_GSK_A, gsk_a);
        ret |= atto640d_i2c_write_reg(ATTO640D_REG_DAC_GSK_B, gsk_b);
        if (ret) {
            LOG_ERR("Auto-Level: GSK schreiben fehlgeschlagen: %d", ret);
            return -EIO;
        }

        /* Schritt 2: Warten bis neuer GSK-Wert im Sensor wirkt */
        k_sleep(K_MSEC(500U));

        /* Schritt 3: Frischen Frame vom DCMIPP holen */
        uint8_t  *frame_data;
        uint32_t  frame_size;
        ret = dcmipp_capture_get_frame(&frame_data, &frame_size, 300);
        if (ret != 0) {
            LOG_ERR("Auto-Level: Frame-Timeout iter=%d: %d", iter, ret);
            return -ETIMEDOUT;
        }

        /* Schritt 4: D-Cache explizit invalidieren (DMA hat direkt in SRAM geschrieben) */
        sys_cache_data_invd_range(frame_data, frame_size);

        /* Schritt 5: Subsampled Average — jedes 10. Pixel, Spalten 0-5 überspringen */
        const uint16_t *px   = (const uint16_t *)frame_data;
        uint32_t        npix = frame_size / sizeof(uint16_t);
        uint64_t        sum  = 0;
        uint32_t        n    = 0;

        for (uint32_t i = 0; i < npix; i += 10U) {
            if ((i % DCMIPP_FRAME_WIDTH) < 6U) {
                continue;
            }
            sum += (uint32_t)(px[i] & 0x3FFFU);
            n++;
        }
        if (n == 0U) {
            return -EINVAL;
        }

        uint32_t avg   = (uint32_t)(sum / n);
        int32_t  error = (int32_t)avg - (int32_t)AUTO_LEVEL_TARGET;
        int32_t  abs_e = error < 0 ? -error : error;

        LOG_INF("Auto-Level [%2d]: GSK=0x%03X (%u)  AVG=%u  err=%d",
                iter, gsk, gsk, avg, error);

        /* Konvergenz erreicht? */
        if ((uint32_t)abs_e <= AUTO_LEVEL_TOL) {
            LOG_INF("Auto-Level: Konvergiert — GSK=0x%03X (%u), AVG=%u",
                    gsk, gsk, avg);
            return 0;
        }

        /* Schritt 6: GSK für nächste Iteration anpassen.
         * ATTO640D ADC INVERTIERT (AD9649 Analog Mode):
         *   höherer GSK → niedrigerer ADC-Wert
         *   niedrigerer GSK → höherer ADC-Wert
         * Daher: error > 0 (avg zu hoch) → GSK erhöhen
         *         error < 0 (avg zu niedrig) → GSK senken
         * Adaptiver Step: weit vom Ziel → grosse Schritte, nah → fein */
        int32_t abs_e2  = error < 0 ? -error : error;
        int32_t divisor, max_step;
        if (abs_e2 > 2000) {
            divisor = 32;  max_step = 64;
        } else if (abs_e2 > 500) {
            divisor = 64;  max_step = 16;
        } else if (abs_e2 > 200) {
            divisor = 256; max_step = 2;
        } else {
            divisor = 512; max_step = 1;
        }
        int32_t step = error / divisor;
        if (step == 0) {
            step = (error > 0) ? 1 : -1;
        } else if (step >  max_step) {
            step =  max_step;
        } else if (step < -max_step) {
            step = -max_step;
        }

        int32_t new_gsk = (int32_t)gsk + step;  /* + wegen invertiertem ADC */
        if (new_gsk < 0)    { new_gsk = 0; }
        if (new_gsk > 1023) { new_gsk = 1023; }
        gsk = (uint16_t)new_gsk;
    }

    LOG_WRN("Auto-Level: Nicht konvergiert nach %d Iterationen", AUTO_LEVEL_MAX_ITER);
    return -EAGAIN;
}

/* ===== Sensor-Info (einmalig nach Init) ===== */

int atto640d_read_sensor_info(float vtemp_mv, bool vtemp_valid)
{
    uint8_t sn_a, sn_b, sn_c, sn_d;
    uint8_t roic_rev, gain_img, int_a, int_b, cfg_a, status;
    int ret;

    /* Seriennummer — 28-bit über 4 Register */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_SERIAL_NB_A, &sn_a);
    if (ret) goto err;
    ret = atto640d_i2c_read_reg(ATTO640D_REG_SERIAL_NB_B, &sn_b);
    if (ret) goto err;
    ret = atto640d_i2c_read_reg(ATTO640D_REG_SERIAL_NB_C, &sn_c);
    if (ret) goto err;
    ret = atto640d_i2c_read_reg(ATTO640D_REG_SERIAL_NB_D, &sn_d);
    if (ret) goto err;

    uint32_t nb1 = ((uint32_t)(sn_a & 0x1FU) << 8) | sn_b;   /* 13-bit */
    uint32_t nb2 = (sn_c >> 2) & 0x1FU;                        /*  5-bit */
    uint32_t nb3 = ((uint32_t)(sn_c & 0x03U) << 8) | sn_d;    /* 10-bit */
    LOG_INF("ATTO640D S/N: %u-%u-%u", nb1, nb2, nb3);

    /* ROIC Revision */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_ROIC_REV, &roic_rev);
    if (ret) goto err;
    LOG_INF("ATTO640D ROIC Rev: 0x%02X", roic_rev);

    /* Gain (Bits[6:4] von GAIN_IMAGE) */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_GAIN_IMAGE, &gain_img);
    if (ret) goto err;
    LOG_INF("ATTO640D Gain: %u (GAIN_IMAGE=0x%02X)", (gain_img >> 4) & 0x7U, gain_img);

    /* Integrationszeit — 10-bit: INT_A[1:0] + INT_B[7:0] */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_INTEGRATION_A, &int_a);
    if (ret) goto err;
    ret = atto640d_i2c_read_reg(ATTO640D_REG_INTEGRATION_B, &int_b);
    if (ret) goto err;
    {
        uint32_t tint = ((uint32_t)(int_a & 0x03U) << 8) | int_b;
        LOG_INF("ATTO640D TINT: %u (0x%03X)", tint, tint);
    }

    /* VTEMP_D_OFFSET — werkseitig kalibrierter 14-bit Signed-Wert */
    {
        uint8_t ofs_a, ofs_b;

        ret = atto640d_i2c_read_reg(ATTO640D_REG_VTEMP_D_OFFS_A, &ofs_a);
        if (ret) goto err;
        ret = atto640d_i2c_read_reg(ATTO640D_REG_VTEMP_D_OFFS_B, &ofs_b);
        if (ret) goto err;

        int16_t vtemp_offset = (int16_t)(((uint16_t)(ofs_a & 0x3FU) << 8) | ofs_b);
        if (vtemp_offset & 0x2000) {
            vtemp_offset |= (int16_t)0xC000;  /* Sign-Extension 14→16 bit */
        }
        LOG_INF("ATTO640D VTEMP_D_OFFSET: %d (raw: A=0x%02X B=0x%02X)",
                vtemp_offset, ofs_a, ofs_b);

        /* FPA Junction-Temperatur aus VTEMP_P: T[°C] = -187.37 × V[V] + 412.5 */
        if (vtemp_valid) {
            double vtemp_v = (double)vtemp_mv / 1000.0;
            double t_junction = -187.37 * vtemp_v + 412.5;
            LOG_INF("ATTO640D FPA Junction Temp (VTEMP_P): %.1f C (V=%.4f V)",
                    t_junction, vtemp_v);
        } else {
            LOG_INF("ATTO640D FPA Junction Temp: nicht berechenbar (ADS1110 nicht verfügbar)");
        }
    }

    /* CONFIG_A: Standby[0], PowerDown[1], VTEMP_EN[2] (gem. UG038) */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_CONFIG_A, &cfg_a);
    if (ret) goto err;
    LOG_INF("ATTO640D CONFIG_A: 0x%02X (Standby=%u PwrDwn=%u VTEMP_EN=%u)",
            cfg_a,
            (cfg_a >> 0) & 1U,
            (cfg_a >> 1) & 1U,
            (cfg_a >> 2) & 1U);

    /* STATUS */
    ret = atto640d_i2c_read_reg(ATTO640D_REG_STATUS, &status);
    if (ret) goto err;
    LOG_INF("ATTO640D STATUS: 0x%02X (ROIC_INIT_DONE=%u SEQ_STATUS=%u)",
            status,
            (status & ATTO640D_STATUS_ROIC_INIT_DONE) ? 1U : 0U,
            (status & ATTO640D_STATUS_SEQ_STATUS)     ? 1U : 0U);

    return ATTO640D_OK;

err:
    LOG_ERR("ATTO640D: Sensor-Info lesen fehlgeschlagen: %d", ret);
    return ATTO640D_ERR_I2C;
}

/* ===== Shutdown-Sequenz (UG038 Table 34) ===== */

/* ===== Zyklische Abfrage-Hilfsfunktionen ===== */

int atto640d_read_id_regs(uint8_t *a, uint8_t *b, uint8_t *c)
{
    int ret = atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_A, a);
    if (ret != 0) return ret;
    ret = atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_B, b);
    if (ret != 0) return ret;
    return atto640d_i2c_read_reg(ATTO640D_REG_READ_ONLY_C, c);
}

int atto640d_read_status_reg(uint8_t *val)
{
    return atto640d_i2c_read_reg(ATTO640D_REG_STATUS, val);
}

void atto640d_shutdown(void)
{
    LOG_INF("ATTO640D: Shutdown-Sequenz (UG038 Table 34)");

    /* Step 1: NRST = 0 (FPA in Reset versetzen) */
    LOG_INF("ATTO640D: Shutdown Step 1 — NRST = 0");
    gpio_pin_set_dt(&nrst_pin, 0);

    /* Step 2: Master Clock stoppen */
    LOG_INF("ATTO640D: Shutdown Step 2 — Master Clock stoppen");
    atto640d_stop_mc();

    /* Step 3: DVDD aus (PE14 → ADP150 Regler aus) */
    LOG_INF("ATTO640D: Shutdown Step 3 — VDDA-EN = 0 (1.8V aus)");
    gpio_pin_set_dt(&vdda_en_pin, 0);

    /* Step 4: AVDD aus (PE8 → ADP7118 Regler aus) */
    LOG_INF("ATTO640D: Shutdown Step 4 — AVDD-EN = 0 (3.75V aus)");
    gpio_pin_set_dt(&avdd_en_pin, 0);
}
